#include "common.h"

ssize_t read_full(int fd, void *buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(fd, (char *)buf + got, n - got);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (r == 0)
      return (ssize_t)got;
    got += (size_t)r;
  }
  return (ssize_t)got;
}

ssize_t write_full(int fd, const void *buf, size_t n) {
  size_t sent = 0;
  while (sent < n) {
    ssize_t r = write(fd, (const char *)buf + sent, n - sent);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    sent += (size_t)r;
  }
  return (ssize_t)sent;
}