#ifndef CONTAINER_H
#define CONTAINER_H

#include "common.h"

typedef struct {
  const char *rootfs;
  const char *overlay_upper; /* NULL if not using overlay */
  const char *overlay_work;
  const char *overlay_merged;
  char *const *argv;
  int pty_slave;   /* -1 if no TTY */
  int stdout_pipe; /* -1 if using PTY */
  int sync_pipe;   /* child blocks here until parent releases */
  int winsize_row;
  int winsize_col;
  int netns_fd;
} child_args_t;

/* Spawn container. Parent must write uid/gid maps (when rootless) and then
 * write one byte to *sync_write_end to release the child.
 * Returns child PID (>0) or -1 on error. */
pid_t container_spawn(child_args_t *args, int *sync_write_end, int rootless);

#endif