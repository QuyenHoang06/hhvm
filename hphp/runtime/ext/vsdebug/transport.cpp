/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2017-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/ext/vsdebug/transport.h"
#include "hphp/runtime/ext/vsdebug/debugger.h"

namespace HPHP {
namespace VSDEBUG {

DebugTransport::DebugTransport(Debugger* debugger) :
  m_debugger(debugger),
  m_outputThread(this, &DebugTransport::processOutgoingMessages),
  m_inputThread(this, &DebugTransport::processIncomingMessages) {
}

void DebugTransport::setTransportFd(int fd) {
  Lock lock(m_mutex);

  // We shouldn't have a valid transport already.
  assert(m_transportFd < 0);
  assert(m_abortPipeFd[0] == -1 && m_abortPipeFd[1] == -1);

  // Create a set of pipe file descriptors to use to inform the thread
  // polling for reads that it's time to exit.
  if (pipe(m_abortPipeFd) < 0) {
    VSDebugLogger::Log(
      VSDebugLogger::LogLevelError,
      "Failed to open pipe for transport termination event."
    );

    // This is unexpected and treated as fatal because we won't be able
    // to stop the polling threads in an orderly fashion at this point.
    assert(false);
  }

  m_transportFd = fd;
  m_terminating = false;
  m_inputThread.start();
  m_outputThread.start();
}

void DebugTransport::shutdownInputThread() {
  // Singal to the read thread that we are shutting down by writing to
  // the abort pipe.
  char value = '\0';
  write(m_abortPipeFd[1], &value, 1);
  close(m_abortPipeFd[1]);
  m_abortPipeFd[1] = -1;
}

void DebugTransport::shutdownOutputThread() {
  // Clear any pending outgoing messages, and wake up the outgoing
  // message thread so that it exits.
  std::unique_lock<std::mutex> lock(m_outgoingMsgLock);
  m_terminating = true;
  m_outgoingMessages.clear();
  m_outgoingMsgCondition.notify_all();
}

void DebugTransport::shutdown() {
  shutdownInputThread();
  shutdownOutputThread();

  // Wait for both threads to exit.
  m_inputThread.waitForEnd();
  m_outputThread.waitForEnd();

  // Cleanup all fds.
  {
    Lock lock(m_mutex);
    close(m_transportFd);
    m_transportFd = -1;
  }
}

void DebugTransport::onClientDisconnected() {
  m_debugger->setClientConnected(false);
}

const std::string DebugTransport::wrapOutgoingMessage(
  folly::dynamic& message,
  const char* messageType
) const {
  // TODO: (Ericblue) output messages need to be encoded in UTF-8
  message["type"] = messageType;
  return folly::toJson(message);
}

void DebugTransport::enqueueOutgoingUserMessage(
  const char* message,
  const char* level
) {
  folly::dynamic userMessage = folly::dynamic::object;

  userMessage["category"] = level;
  userMessage["output"] = message;
  enqueueOutgoingEventMessage(userMessage, EventTypeOutput);
}

void DebugTransport::enqueueOutgoingEventMessage(
  folly::dynamic& message,
  const char* eventType
) {
  folly::dynamic eventMessage = folly::dynamic::object;
  eventMessage["event"] = eventType;
  eventMessage["body"] = message;
  enqueueOutgoingMessageForClient(eventMessage, MessageTypeEvent);
}

void DebugTransport::enqueueOutgoingMessageForClient(
  folly::dynamic& message,
  const char* messageType
) {
  if (!clientConnected()) {
    return;
  }

  const auto wrapped = wrapOutgoingMessage(message, messageType);

  {
    std::lock_guard<std::mutex> lock(m_outgoingMsgLock);
    m_outgoingMessages.push_back(wrapped);
    m_outgoingMsgCondition.notify_all();
  }
}

void DebugTransport::processOutgoingMessages() {
  VSDebugLogger::Log(
    VSDebugLogger::LogLevelInfo,
    "Transport outgoing message thread started."
  );

  SCOPE_EXIT {
    VSDebugLogger::Log(
      VSDebugLogger::LogLevelInfo,
      "Transport outgoing message thread exiting."
    );
  };

  int fd = getTransportFd();
  if (fd < 0) {
    return;
  }

  while (true) {
    std::list<std::string> messagesToSend;
    {
      // Take a local copy of any messages waiting to be sent under the
      // lock and clear the queue.
      std::unique_lock<std::mutex> lock(m_outgoingMsgLock);
      while (!m_terminating && m_outgoingMessages.size() == 0) {
        m_outgoingMsgCondition.wait(lock);
      }

      if (m_terminating) {
        return;
      }

      messagesToSend = std::list<std::string>(m_outgoingMessages);
      m_outgoingMessages.clear();
    }

    // Send the messages.
    for (auto it = messagesToSend.begin();
         it != messagesToSend.end();
         it++) {

      // Write out the entire string, *including* its terminating NULL char.
      const char* output = it->c_str();
      if (write(fd, output, strlen(output) + 1) < 0) {
        VSDebugLogger::Log(
          VSDebugLogger::LogLevelError,
          "Sending message failed:\n%s\nWrite returned %d",
          output,
          errno
        );
        onClientDisconnected();
        return;
      }
    }
  }
}

void DebugTransport::processIncomingMessages() {
  VSDebugLogger::Log(
    VSDebugLogger::LogLevelInfo,
    "Transport incoming message thread started."
  );

  char* buffer = nullptr;
  size_t bufferSize = 0;
  size_t bufferPosition = 0;

  int fd = getTransportFd();
  if (fd < 0) {
    return;
  }

  // Wait for data to be available, or a termination event to occur.
  constexpr int abortIdx = 0;
  constexpr int transportIdx = 1;
  int eventMask = POLLIN | POLLERR | POLLHUP;
  struct pollfd pollFds[2];
  memset(pollFds, 0, sizeof(pollFds));
  pollFds[abortIdx].fd = m_abortPipeFd[0];
  pollFds[abortIdx].events = eventMask;
  pollFds[transportIdx].fd = fd;
  pollFds[transportIdx].events = eventMask;

  while (true) {
    // If there are complete messages in the buffer, process them first.
    folly::dynamic message;
    while (bufferPosition > 0 &&
           tryProcessMessage(buffer, bufferSize, &bufferPosition, &message)) {

      // Call the debugger and have it process the client message.
      m_debugger->onClientMessage(message);
    }

    // Out of space in the buffer. Attempt to resize it.
    if (bufferPosition == bufferSize) {
      const size_t newSize = buffer == nullptr
        ? ReadBufferDefaultSize
        : bufferSize * 2;

      buffer = (char*)realloc(buffer, newSize);
      if (buffer == nullptr) {
        VSDebugLogger::Log(
          VSDebugLogger::LogLevelError,
          "Transport incoming message thread: out of memory!"
        );
        break;
      }

      memset(&buffer[bufferSize], 0, newSize - bufferSize);
      bufferSize = newSize;
    }

    int ret = poll(pollFds, 2, -1);
    if (ret < 0) {
      if (ret == -EINTR) {
        // Interrupted syscall, resume polling.
        continue;
      }

      VSDebugLogger::Log(
        VSDebugLogger::LogLevelError,
        "Polling inputs failed: %d (%s)",
        errno,
        strerror(errno)
      );
      break;
    }

    if (pollFds[abortIdx].revents != 0) {
      // Termination event received.
      VSDebugLogger::Log(
        VSDebugLogger::LogLevelInfo,
        "Transport read thread: termination signal received."
      );
      break;
    } else if ((pollFds[transportIdx].revents & POLLIN) == 0) {
      // This means that the client has disconnected.
      VSDebugLogger::Log(
        VSDebugLogger::LogLevelInfo,
        "Transport read thread: client disconnected (event mask = 0x%x).",
        pollFds[transportIdx].revents
      );
      break;
    }

    // Read the next chunk of data from the pipe.
    const size_t readSize = bufferSize - bufferPosition;
    const ssize_t result = recv(
      fd,
      &buffer[bufferPosition],
      readSize,
      0);

    if (result <= 0) {
      if (result == -EINTR) {
        // Interrupted syscall, retry recv.
        continue;
      }

      // Log the result of recv, unless it's 0 which indicates an orderly
      // shutdown by the peer.
      if (result != 0) {
        VSDebugLogger::Log(
          VSDebugLogger::LogLevelError,
          "Transport incoming message thread failed to read: %d",
          result
        );
      }
      break;
    }

    bufferPosition += result;
    pollFds[abortIdx].revents = 0;
    pollFds[transportIdx].revents = 0;
  }

  if (buffer != nullptr) {
    free(buffer);
  }

  // Close the read end of the pipe.
  close(m_abortPipeFd[0]);
  m_abortPipeFd[0] = -1;

  VSDebugLogger::Log(
    VSDebugLogger::LogLevelInfo,
    "Transport incoming message thread exiting."
  );

  onClientDisconnected();
}

bool DebugTransport::tryProcessMessage(
  char* buffer,
  size_t bufferSize,
  size_t* bufferPosition,
  folly::dynamic* message
) {
  bool success = false;

  const char* bufferPos = buffer + *bufferPosition;
  assert(bufferPos <= buffer + bufferSize);

  // Advance through the buffer until we locate the NULL separator between
  // client messages.
  char* currentPos = buffer;
  while (currentPos < bufferPos && *currentPos != '\0') {
    currentPos++;
  }

  // If we did not reach the end of the available input, and a NULL
  // character was encountered, attempt to parse the message.
  if (currentPos < bufferPos && *currentPos == '\0') {
    try {
      *message = folly::parseJson(buffer);
      success = true;
    } catch (...) {
      // Log the error and move on. Note that in this case we cannot even
      // send a failure response to the debugger client because the protocol
      // requires it to include a sequence ID and the command that failed -
      // and request message was not well-formed enough for us to obtain that
      // data from it.
      VSDebugLogger::Log(
        VSDebugLogger::LogLevelError,
        "Failed to parse debugger message: %s",
        buffer
      );
    }

    // Lose the NULL character.
    currentPos++;

    // If there's remaining data in the buffer after this message ended,
    // it remains for the next message.
    if (currentPos < bufferPos) {
      size_t len = bufferPos - currentPos;
      memmove(buffer, currentPos, len);
      memset(buffer + len, 0, bufferSize - len);
      *bufferPosition = len;
    } else {
      memset(buffer, 0, bufferSize);
      *bufferPosition = 0;
    }
  }

  return success;
}

}
}
