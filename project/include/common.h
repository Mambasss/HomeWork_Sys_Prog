// #ifndef COMMON_H
#define COMMON_H

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/sched.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define SOCK_PATH "/tmp/minidocker.sock"
#define BASE_DIR "/tmp/minidocker"
#define LOG_DIR BASE_DIR "/logs"
#define OVERLAY_DIR BASE_DIR "/overlay"

#define ID_LEN 64
#define CMD_LEN 1024
#define PATH_LEN 512
#define MAX_CONTAINERS 64
#define MAX_ATTACH 8

#define ST_RUNNING 0
#define ST_EXITED 1

/* Sockets helpers (defined in daemon.c, used by client too via extern) */
ssize_t read_full(int fd, void *buf, size_t n);
ssize_t write_full(int fd, const void *buf, size_t n);

/* Protocol:
 *   Request  = [uint32_be len][payload]
 *              payload = null-separated fields. Field 0 = command.
 *   Response = [uint32_be len][payload]  ; payload starts with "OK" or "ERR "
 *   For ATTACH/RUN (foreground TTY), after the OK response the daemon sends
 *   a single-byte sendmsg() carrying the socketpair FD via SCM_RIGHTS.
 */

// #endif