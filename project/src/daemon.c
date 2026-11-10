#include "common.h"
#include "container.h"

typedef struct container {
  char id[ID_LEN];
  char cmd[CMD_LEN];
  char rootfs[PATH_LEN];
  pid_t host_pid;
  int state;
  int exit_code;
  time_t created;
  int pty_master; /* PTY master OR pipe read-end */
  int log_fd;
  char log_path[PATH_LEN];
  int is_tty;
  int attach_fds[MAX_ATTACH];
  int n_attach;
  int follow_fds[MAX_ATTACH];
  int n_follow;
  struct container *next;
} container_t;

static container_t *containers = NULL;
static int listen_fd = -1;
static volatile sig_atomic_t got_sigchld = 0;

static void on_sigchld(int s) {
  (void)s;
  got_sigchld = 1;
}

/* ---------- helpers ---------- */
static container_t *find_container(const char *id) {
  for (container_t *c = containers; c; c = c->next)
    if (!strcmp(c->id, id))
      return c;
  return NULL;
}

static container_t *find_by_pid(pid_t pid) {
  for (container_t *c = containers; c; c = c->next)
    if (c->host_pid == pid)
      return c;
  return NULL;
}

static void container_close_fds(container_t *c) {
  if (c->pty_master >= 0) {
    close(c->pty_master);
    c->pty_master = -1;
  }
  for (int i = 0; i < c->n_attach; i++) {
    if (c->attach_fds[i] >= 0)
      close(c->attach_fds[i]);
    c->attach_fds[i] = -1;
  }
  c->n_attach = 0;
  for (int i = 0; i < c->n_follow; i++) {
    if (c->follow_fds[i] >= 0)
      close(c->follow_fds[i]);
    c->follow_fds[i] = -1;
  }
  c->n_follow = 0;
}

static void reap_children(void) {
  int status;
  pid_t pid;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    container_t *c = find_by_pid(pid);
    if (c) {
      c->state = ST_EXITED;
      c->exit_code =
          WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      container_close_fds(c);
    }
  }
}

/* ---------- protocol ---------- */
static int send_payload(int fd, const char *payload, size_t len) {
  uint32_t lb = htonl((uint32_t)len);
  if (write_full(fd, &lb, 4) < 0)
    return -1;
  if (len && write_full(fd, payload, len) < 0)
    return -1;
  return 0;
}

static int send_error(int fd, const char *msg) {
  char buf[512];
  int n = snprintf(buf, sizeof(buf), "ERR %s", msg);
  return send_payload(fd, buf, n);
}

static int send_ok(int fd, const char *data) {
  char buf[512];
  int n = snprintf(buf, sizeof(buf), "OK %s", data ? data : "");
  return send_payload(fd, buf, n);
}

static int send_fd(int sock, int fd_to_send) {
  char dummy = 0;
  struct iovec iov = {.iov_base = &dummy, .iov_len = 1};
  char cmsgbuf[CMSG_SPACE(sizeof(int))];
  struct msghdr msg = {
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = cmsgbuf,
      .msg_controllen = sizeof(cmsgbuf),
  };
  struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
  cm->cmsg_level = SOL_SOCKET;
  cm->cmsg_type = SCM_RIGHTS;
  cm->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cm), &fd_to_send, sizeof(int));
  return sendmsg(sock, &msg, 0) > 0 ? 0 : -1;
}

/* Read a request. Returns malloc'd payload (caller frees), sets *len. */
static char *read_request(int fd, uint32_t *len_out) {
  uint32_t lb;
  if (read_full(fd, &lb, 4) != 4)
    return NULL;
  uint32_t len = ntohl(lb);
  if (len == 0 || len > 65536)
    return NULL;
  char *buf = malloc(len + 1);
  if (!buf)
    return NULL;
  if (read_full(fd, buf, len) != (ssize_t)len) {
    free(buf);
    return NULL;
  }
  buf[len] = 0;
  *len_out = len;
  return buf;
}

/* ---------- container helpers ---------- */
static void write_log(container_t *c, const char *buf, size_t n) {
  if (c->log_fd >= 0) {
    if (write_full(c->log_fd, buf, n) < 0) { /* ignore */
    }
  }
  /* fan out to followers */
  for (int i = 0; i < c->n_follow; i++) {
    if (write_full(c->follow_fds[i], buf, n) < 0) {
      close(c->follow_fds[i]);
      c->follow_fds[i] = c->follow_fds[c->n_follow - 1];
      c->n_follow--;
      i--;
    }
  }
  /* fan out to attach clients */
  for (int i = 0; i < c->n_attach; i++) {
    if (write_full(c->attach_fds[i], buf, n) < 0) {
      close(c->attach_fds[i]);
      c->attach_fds[i] = c->attach_fds[c->n_attach - 1];
      c->n_attach--;
      i--;
    }
  }
}

static void kill_container(container_t *c) {
  if (c->state != ST_RUNNING)
    return;
  kill(c->host_pid, SIGTERM);
  for (int i = 0; i < 30; i++) {
    int status;
    pid_t p = waitpid(c->host_pid, &status, WNOHANG);
    if (p == c->host_pid) {
      c->state = ST_EXITED;
      c->exit_code =
          WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      container_close_fds(c);
      return;
    }
    usleep(100 * 1000);
  }
  kill(c->host_pid, SIGKILL);
  int status;
  if (waitpid(c->host_pid, &status, 0) == c->host_pid) {
    c->state = ST_EXITED;
    c->exit_code =
        WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    container_close_fds(c);
  }
}

/* ---------- command handlers ---------- */
static void cmd_run(int conn, char *req, uint32_t len) {
  char rootfs[PATH_LEN] = "";
  char name[ID_LEN] = "";
  char net_from[ID_LEN] = "";
  int tty = 0, detach = 0, overlay = 0, ws_row = 24, ws_col = 80;
  char **argv = NULL;
  int argc = 0, cap = 0;

  char *p = req, *end = req + len;
  /* skip "RUN" */
  p += strlen(p) + 1;
  while (p < end) {
    char *f = p;
    char *z = memchr(p, 0, end - p);
    if (!z)
      break;
    p = z + 1;
    if (!strcmp(f, "argv")) {
      while (p < end) {
        char *af = p;
        char *az = memchr(p, 0, end - p);
        if (!az)
          break;
        p = az + 1;
        if (argc == cap) {
          cap = cap ? cap * 2 : 8;
          argv = realloc(argv, cap * sizeof(char *));
        }
        argv[argc++] = af;
      }
      break;
    } else if (!strncmp(f, "rootfs=", 7))
      strncpy(rootfs, f + 7, PATH_LEN - 1);
    else if (!strncmp(f, "name=", 5))
      strncpy(name, f + 5, ID_LEN - 1);
    else if (!strncmp(f, "tty=", 4))
      tty = atoi(f + 4);
    else if (!strncmp(f, "detach=", 7))
      detach = atoi(f + 7);
    else if (!strncmp(f, "overlay=", 8))
      overlay = atoi(f + 8);
    else if (!strncmp(f, "network=", 8)) {
      const char *v = f + 8;
      if (!strncmp(v, "container:", 10))
        snprintf(net_from, sizeof(net_from), "%s", v + 10);
    } else if (!strncmp(f, "row=", 4))
      ws_row = atoi(f + 4);
    else if (!strncmp(f, "col=", 4))
      ws_col = atoi(f + 4);
  }

  if (!argc || !rootfs[0]) {
    send_error(conn, "missing argv or rootfs");
    free(argv);
    return;
  }
  argv = realloc(argv, (argc + 1) * sizeof(char *));
  argv[argc] = NULL;

  int netns_fd = -1;
  if (net_from[0]) {
    container_t *parent = find_container(net_from);
    if (!parent || parent->state != ST_RUNNING) {
      send_error(conn, "parent container not running");
      free(argv);
      return;
    }
    char p[64];
    snprintf(p, sizeof(p), "/proc/%d/ns/net", parent->host_pid);
    netns_fd = open(p, O_RDONLY);
    if (netns_fd < 0) {
      send_error(conn, "cannot open parent netns");
      free(argv);
      return;
    }
  }

  container_t *c = calloc(1, sizeof(*c));
  if (!c) {
    send_error(conn, "oom");
    free(argv);
    return;
  }

  if (name[0])
    snprintf(c->id, ID_LEN, "%s", name);
  else
    snprintf(c->id, ID_LEN, "c-%08x", (unsigned)(time(NULL) ^ getpid()));

  int off = 0;
  for (int i = 0; i < argc && off < CMD_LEN - 1; i++)
    off += snprintf(c->cmd + off, CMD_LEN - off, "%s%s", i ? " " : "", argv[i]);

  snprintf(c->rootfs, PATH_LEN, "%s", rootfs);
  c->created = time(NULL);
  c->state = ST_RUNNING;
  c->host_pid = -1;
  c->pty_master = -1;
  c->log_fd = -1;
  c->is_tty = tty;

  snprintf(c->log_path, PATH_LEN, "%s/%s.log", LOG_DIR, c->id);
  c->log_fd = open(c->log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

  int pty_master = -1, pty_slave = -1;
  if (tty) {
    pty_master = posix_openpt(O_RDWR | O_NOCTTY);
    if (pty_master < 0 || grantpt(pty_master) < 0 || unlockpt(pty_master) < 0) {
      send_error(conn, "pty setup failed");
      free(argv);
      return;
    }
    char *slave_name = ptsname(pty_master);
    pty_slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (pty_slave < 0) {
      send_error(conn, "pty slave open");
      free(argv);
      return;
    }
    struct winsize ws = {.ws_row = ws_row, .ws_col = ws_col};
    ioctl(pty_master, TIOCSWINSZ, &ws);
  }

  int stdout_pipe[2] = {-1, -1};
  if (!tty) {
    if (pipe(stdout_pipe) < 0) {
      send_error(conn, "pipe");
      free(argv);
      return;
    }
  }

  char ov_upper[PATH_LEN + 16] = "", ov_work[PATH_LEN + 16] = "",
                           ov_merged[PATH_LEN + 16] = "";
  if (overlay) {
    char d[PATH_LEN];
    snprintf(d, PATH_LEN, "%s/%s", OVERLAY_DIR, c->id);
    mkdir(OVERLAY_DIR, 0755);
    mkdir(d, 0755);
    snprintf(ov_upper, sizeof(ov_upper), "%s/upper", d);
    snprintf(ov_work, sizeof(ov_work), "%s/work", d);
    snprintf(ov_merged, sizeof(ov_merged), "%s/merged", d);
    mkdir(ov_upper, 0755);
    mkdir(ov_work, 0755);
    mkdir(ov_merged, 0755);
  }

  child_args_t ca = {
      .rootfs = rootfs,
      .overlay_upper = overlay ? ov_upper : NULL,
      .overlay_work = overlay ? ov_work : NULL,
      .overlay_merged = overlay ? ov_merged : NULL,
      .argv = argv,
      .pty_slave = pty_slave,
      .stdout_pipe = stdout_pipe[1],
      .sync_pipe = -1,
      .winsize_row = ws_row,
      .winsize_col = ws_col,
      .netns_fd = netns_fd,
  };

  int sync_w = -1;
  pid_t pid = container_spawn(&ca, &sync_w, geteuid() != 0);

  if (netns_fd >= 0)
    close(netns_fd);

  if (pid < 0) {
    send_error(conn, "spawn failed");
    if (pty_master >= 0)
      close(pty_master);
    if (pty_slave >= 0)
      close(pty_slave);
    if (stdout_pipe[0] >= 0) {
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
    }
    free(argv);
    return;
  }

  c->host_pid = pid;
  c->pty_master = tty ? pty_master : stdout_pipe[0];

  if (pty_slave >= 0)
    close(pty_slave);
  if (stdout_pipe[1] >= 0)
    close(stdout_pipe[1]);

  /* release child */
  if (write(sync_w, "x", 1) < 0) { /* ignore */
  }
  close(sync_w);

  c->next = containers;
  containers = c;

  if (detach) {
    send_ok(conn, c->id);
    close(conn);
  } else {
    /* foreground: send OK + socketpair fd */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
      send_error(conn, "socketpair");
      close(conn);
      return;
    }
    send_ok(conn, c->id);
    if (send_fd(conn, sv[0]) < 0) {
      close(sv[0]);
      close(sv[1]);
      close(conn);
      return;
    }
    close(sv[0]);
    if (c->n_attach < MAX_ATTACH)
      c->attach_fds[c->n_attach++] = sv[1];
    else
      close(sv[1]);
    close(conn);
  }
  free(argv);
}

static void cmd_list(int conn) {
  size_t cap = 4096, used = 0;
  char *buf = malloc(cap);
  for (container_t *c = containers; c; c = c->next) {
    char line[CMD_LEN + 128];
    int n = snprintf(line, sizeof(line), "%s %d %s %s\n", c->id, c->host_pid,
                     c->state == ST_RUNNING ? "running" : "exited", c->cmd);
    while (used + (size_t)n + 1 > cap) {
      cap *= 2;
      buf = realloc(buf, cap);
    }
    memcpy(buf + used, line, n);
    used += n;
  }
  char *out = malloc(used + 4);
  memcpy(out, "OK ", 3);
  memcpy(out + 3, buf, used);
  send_payload(conn, out, used + 3);
  free(out);
  free(buf);
  close(conn);
}

static void cmd_kill(int conn, const char *id) {
  container_t *c = find_container(id);
  if (!c) {
    send_error(conn, "not found");
    close(conn);
    return;
  }
  kill_container(c);
  char buf[32];
  snprintf(buf, sizeof(buf), "%d", c->exit_code);
  send_ok(conn, buf);
  close(conn);
}

static void cmd_logs(int conn, const char *id, int follow) {
  container_t *c = find_container(id);
  if (!c) {
    send_error(conn, "not found");
    close(conn);
    return;
  }

  int lf = open(c->log_path, O_RDONLY);
  if (lf >= 0) {
    char buf[4096];
    ssize_t r;
    while ((r = read(lf, buf, sizeof(buf))) > 0) {
      send_payload(conn, buf, r);
    }
    close(lf);
  } else {
    send_payload(conn, "", 0);
  }
  if (!follow) {
    close(conn);
    return;
  }

  /* attach as follower: the current conn becomes a raw stream */
  if (c->n_follow < MAX_ATTACH) {
    /* transition to raw: send one zero-length frame, then write bytes directly
     */
    uint32_t zero = 0;
    write_full(conn, &zero, 4); /* marker: no more framed messages */
    c->follow_fds[c->n_follow++] = conn;
  } else {
    close(conn);
  }
}

static void cmd_attach(int conn, const char *id) {
  container_t *c = find_container(id);
  if (c->state != ST_RUNNING) {
    send_error(conn, "container not running");
    close(conn);
    return;
  }
  if (!c) {
    send_error(conn, "not found");
    close(conn);
    return;
  }
  if (!c->is_tty) {
    send_error(conn, "container has no TTY");
    close(conn);
    return;
  }

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
    send_error(conn, "socketpair");
    close(conn);
    return;
  }
  send_ok(conn, c->id);
  if (send_fd(conn, sv[0]) < 0) {
    close(sv[0]);
    close(sv[1]);
    close(conn);
    return;
  }
  close(sv[0]);
  if (c->n_attach < MAX_ATTACH)
    c->attach_fds[c->n_attach++] = sv[1];
  else
    close(sv[1]);
  close(conn);
}

static void cmd_wait(int conn, const char *id) {
  container_t *c = find_container(id);
  if (!c) {
    send_error(conn, "not found");
    close(conn);
    return;
  }
  for (int i = 0; i < 300 && c->state == ST_RUNNING; i++) {
    if (got_sigchld) {
      got_sigchld = 0;
      reap_children();
    }
    usleep(100 * 1000);
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "%d", c->exit_code);
  send_ok(conn, buf);
  close(conn);
}

/* ---------- dispatcher ---------- */
static void handle_conn(int conn) {
  uint32_t len;
  char *req = read_request(conn, &len);
  if (!req) {
    close(conn);
    return;
  }

  char *cmd = req;
  if (!strcmp(cmd, "RUN") || !strcmp(cmd, "run"))
    cmd_run(conn, req, len);
  else if (!strcmp(cmd, "LIST"))
    cmd_list(conn);
  else if (!strcmp(cmd, "KILL")) {
    const char *id = cmd + strlen(cmd) + 1;
    if (id >= req + len) {
      send_error(conn, "missing id");
      close(conn);
    } else
      cmd_kill(conn, id);
  } else if (!strcmp(cmd, "LOGS")) {
    const char *id = cmd + strlen(cmd) + 1;
    int follow = 0;
    const char *p = id + strlen(id) + 1;
    while (p < req + len) {
      if (!strncmp(p, "follow=", 7))
        follow = atoi(p + 7);
      p += strlen(p) + 1;
    }
    cmd_logs(conn, id, follow);
  } else if (!strcmp(cmd, "ATTACH")) {
    const char *id = cmd + strlen(cmd) + 1;
    cmd_attach(conn, id);
  } else if (!strcmp(cmd, "WAIT")) {
    const char *id = cmd + strlen(cmd) + 1;
    cmd_wait(conn, id);
  } else {
    send_error(conn, "unknown command");
    close(conn);
  }
  free(req);
}

/* ---------- main loop ---------- */
int main(void) {
  signal(SIGPIPE, SIG_IGN);
  struct sigaction sa = {.sa_handler = on_sigchld};
  sigaction(SIGCHLD, &sa, NULL);

  mkdir(BASE_DIR, 0755);
  mkdir(LOG_DIR, 0755);

  unlink(SOCK_PATH);
  listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  strcpy(addr.sun_path, SOCK_PATH);
  if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    return 1;
  }
  chmod(SOCK_PATH, 0777);
  listen(listen_fd, 16);

  fprintf(stderr, "minidockerd listening on %s (uid=%d)\n", SOCK_PATH,
          geteuid());

  while (1) {
    if (got_sigchld) {
      got_sigchld = 0;
      reap_children();
    }

    /* build poll set */
    int nfds = 1;
    struct pollfd *fds =
        calloc(1 + MAX_CONTAINERS * (1 + MAX_ATTACH), sizeof(*fds));
    container_t **fdc =
        calloc(1 + MAX_CONTAINERS * (1 + MAX_ATTACH), sizeof(*fdc));
    int *fdt = calloc(1 + MAX_CONTAINERS * (1 + MAX_ATTACH), sizeof(int));

    fds[0].fd = listen_fd;
    fds[0].events = POLLIN;
    fdc[0] = NULL;
    fdt[0] = 0;

    for (container_t *c = containers; c; c = c->next) {
      if (c->state == ST_RUNNING && c->pty_master >= 0) {
        fds[nfds].fd = c->pty_master;
        fds[nfds].events = POLLIN;
        fdc[nfds] = c;
        fdt[nfds] = 1;
        nfds++;
      }
      for (int i = 0; i < c->n_attach; i++) {
        fds[nfds].fd = c->attach_fds[i];
        fds[nfds].events = POLLIN;
        fdc[nfds] = c;
        fdt[nfds] = 2;
        nfds++;
      }
    }

    int n = poll(fds, nfds, 500);
    if (n < 0 && errno != EINTR) {
      perror("poll");
      break;
    }

    for (int i = 0; i < nfds && n > 0; i++) {
      if (!(fds[i].revents & (POLLIN | POLLHUP)))
        continue;
      n--;

      if (fdt[i] == 0) {
        int conn = accept(listen_fd, NULL, NULL);
        if (conn >= 0)
          handle_conn(conn);
      } else if (fdt[i] == 1) {
        /* PTY / pipe readable */
        container_t *c = fdc[i];
        char buf[4096];
        ssize_t r = read(fds[i].fd, buf, sizeof(buf));
        if (r > 0)
          write_log(c, buf, r);
        else if (r == 0 || (r < 0 && errno != EAGAIN)) {
          container_close_fds(c);
        }
      } else if (fdt[i] == 2) {
        /* attach socket -> write to PTY */
        container_t *c = fdc[i];
        char buf[4096];
        ssize_t r = read(fds[i].fd, buf, sizeof(buf));
        if (r > 0) {
          if (c->pty_master >= 0) {
            if (write_full(c->pty_master, buf, r) < 0) { /* ignore */
            }
          }
        } else {
          /* client detached */
          for (int j = 0; j < c->n_attach; j++) {
            if (c->attach_fds[j] == fds[i].fd) {
              close(c->attach_fds[j]);
              c->attach_fds[j] = c->attach_fds[c->n_attach - 1];
              c->n_attach--;
              break;
            }
          }
        }
      }
    }
    free(fds);
    free(fdc);
    free(fdt);
  }
  return 0;
}