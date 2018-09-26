#include "UnixSocketHandler.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <resolv.h>
#include <sys/socket.h>
#include <unistd.h>

namespace et {
UnixSocketHandler::UnixSocketHandler() {}

bool UnixSocketHandler::hasData(int fd) {
  // this mutex is not necessary
  // lock_guard<std::recursive_mutex> guard(mutex);
  fd_set input;
  FD_ZERO(&input);
  FD_SET(fd, &input);
  struct timeval timeout;
  timeout.tv_sec = 0;
  timeout.tv_usec = 0;
  int n = select(fd + 1, &input, NULL, NULL, &timeout);
  if (n == -1) {
    // Select timed out or failed.
    VLOG(4) << "socket select timeout";
    return false;
  } else if (n == 0)
    return false;
  if (!FD_ISSET(fd, &input)) {
    LOG(FATAL) << "FD_ISSET is false but we should have data by now.";
  }
  VLOG(4) << "socket " << fd << " has data";
  return true;
}

ssize_t UnixSocketHandler::read(int fd, void *buf, size_t count) {
  // lock_guard<std::recursive_mutex> guard(mutex);
  // different threads reading different fd
  if (fd <= 0) {
    LOG(FATAL) << "Tried to read from an invalid socket: " << fd;
  }
  if (activeSockets.find(fd) == activeSockets.end()) {
    LOG(INFO) << "Tried to read from a socket that has been closed: " << fd;
    return 0;
  }
  VLOG(4) << "Unixsocket handler read from fd: " << fd;
  ssize_t readBytes = ::read(fd, buf, count);
  if (readBytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    LOG(ERROR) << "Error reading: " << errno << " " << strerror(errno);
  }
  return readBytes;
}

ssize_t UnixSocketHandler::write(int fd, const void *buf, size_t count) {
  // lock_guard<std::recursive_mutex> guard(mutex);
  // different threads writing to different fd
  VLOG(4) << "Unixsocket handler write to fd: " << fd;
  if (fd <= 0) {
    LOG(FATAL) << "Tried to write to an invalid socket: " << fd;
  }
  if (activeSockets.find(fd) == activeSockets.end()) {
    LOG(INFO) << "Tried to write to a socket that has been closed: " << fd;
    return 0;
  }
#ifdef MSG_NOSIGNAL
  return ::send(fd, buf, count, MSG_NOSIGNAL);
#else
  return ::write(fd, buf, count);
#endif
}

void UnixSocketHandler::addToActiveSockets(int fd) {
  if (activeSockets.find(fd) != activeSockets.end()) {
    LOG(FATAL) << "Tried to insert an fd that already exists: " << fd;
  }
  activeSockets.insert(fd);
}

int UnixSocketHandler::accept(int sockfd) {
  lock_guard<std::recursive_mutex> guard(mutex);
  VLOG(3) << "Got mutex when sockethandler accept " << sockfd;
  sockaddr_in client;
  socklen_t c = sizeof(sockaddr_in);
  int client_sock = ::accept(sockfd, (sockaddr *)&client, &c);
  VLOG(3) << "Socket " << sockfd << " accepted, returned client_sock: "
          << client_sock;
  if (client_sock >= 0) {
    initSocket(client_sock);
    addToActiveSockets(client_sock);
    VLOG(3) << "Client_socket inserted to activeSockets";
    return client_sock;
  } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
    FATAL_FAIL(-1);  // LOG(FATAL) with the error
  }

  return -1;
}

void UnixSocketHandler::close(int fd) {
  lock_guard<std::recursive_mutex> guard(mutex);
  if (fd == -1) {
    return;
  }
  if (activeSockets.find(fd) == activeSockets.end()) {
    // Connection was already killed.
    LOG(ERROR) << "Tried to close a connection that doesn't exist: " << fd;
    return;
  }
  activeSockets.erase(activeSockets.find(fd));
  // Shutting down connection before closing to prevent the server
  // from closing it.
  VLOG(1) << "Shutting down connection: " << fd;
  int rc = ::shutdown(fd, SHUT_RDWR);
  if (rc == -1) {
    if (errno == ENOTCONN || errno == EADDRNOTAVAIL) {
      // Shutdown is failing on OS/X with errno (49): Can't assign requested
      // address Possibly an OS bug but I don't think it's necessary anyways.

      // ENOTCONN is harmless
    } else {
      FATAL_FAIL(rc);
    }
  }
  VLOG(1) << "Closing connection: " << fd;
  FATAL_FAIL(::close(fd));
}

}  // namespace et