#include "common.h"

/* ---------- helpers ---------- */
static int connect_daemon(void) {
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  strcpy(a.sun_path, SOCK_PATH);
  if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
    perror("connect");
    close(s);
    return -1;
  }
  return s;
}

static int send_fields(int sock, char **fields, int n) {
  size_t total = 0;
  for (int i = 0; i < n; i++)
    total += strlen(fields[i]) + 1;
  uint32_t lb = htonl((uint32_t)total);
  if (write_full(sock, &lb, 4) < 0)
    return -1;
  for (int i = 0; i < n; i++) {
    size_t l = strlen(fields[i]) + 1;
    if (write_full(sock, fields[i], l) < 0)
      return -1;
  }
  return 0;
}

static char *read_response(int sock) {
  uint32_t lb;
  if (read_full(sock, &lb, 4) != 4)
    return NULL;
  uint32_t len = ntohl(lb);
  char *buf = malloc(len + 1);
  if (!buf)
    return NULL;
  if (len && read_full(sock, buf, len) != (ssize_t)len) {
    free(buf);
    return NULL;
  }
  buf[len] = 0;
  return buf;
}

static int recv_fd(int sock) {
  char buf[1];
  struct iovec iov = {.iov_base = buf, .iov_len = 1};
  char cmsgbuf[CMSG_SPACE(sizeof(int))];
  struct msghdr msg = {
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = cmsgbuf,
      .msg_controllen = sizeof(cmsgbuf),
  };
  ssize_t n = recvmsg(sock, &msg, 0);
  if (n <= 0)
    return -1;
  struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
  if (!cm || cm->cmsg_type != SCM_RIGHTS)
    return -1;
  int fd;
  memcpy(&fd, CMSG_DATA(cm), sizeof(int));
  return fd;
}

/* ---------- interactive I/O ---------- */
static int interactive(int fd, int is_tty) {
  struct termios orig;
  int saved = 0;
  if (is_tty && isatty(STDIN_FILENO)) {
    tcgetattr(STDIN_FILENO, &orig);
    struct termios raw = orig;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    saved = 1;
  }

  struct pollfd fds[2];
  int nfds = 2;
  int stdin_open = is_tty;

  while (1) {
    if (stdin_open) {
      fds[0].fd = STDIN_FILENO;
      fds[0].events = POLLIN;
    } else {
      fds[0].fd = -1;
      fds[0].events = 0;
    }
    fds[1].fd = fd;
    fds[1].events = POLLIN;

    int n = poll(fds, nfds, -1);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      break;
    }

    if (fds[1].revents & (POLLIN | POLLHUP)) {
      char buf[4096];
      ssize_t r = read(fd, buf, sizeof(buf));
      if (r <= 0)
        break;
      if (write_full(STDOUT_FILENO, buf, r) < 0)
        break;
    }
    if (stdin_open && (fds[0].revents & POLLIN)) {
      char buf[4096];
      ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
      if (r <= 0) {
        stdin_open = 0; /* больше не читаем stdin */
      } else if (write_full(fd, buf, r) < 0) {
        break;
      }
    }
  }

  if (saved)
    tcsetattr(STDIN_FILENO, TCSANOW, &orig);
  return 0;
}

/* ---------- commands ---------- */
static int do_run(int argc, char **argv, int tty, int detach,
                  const char *rootfs, const char *name, int overlay,
                  const char *network) {
  int sock = connect_daemon();
  if (sock < 0)
    return 1;

  /* gather tty size */
  int row = 24, col = 80;
  if (isatty(STDIN_FILENO)) {
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
      row = ws.ws_row;
      col = ws.ws_col;
    }
  }

  char rf[PATH_LEN], tf[16], df[16], of[24], nf[ID_LEN + 8];
  char rowf[32], colf[32];
  char nwf[ID_LEN + 16];
  snprintf(nwf, sizeof(nwf), "network=%s", network ? network : "");
  snprintf(rf, sizeof(rf), "rootfs=%s", rootfs);
  snprintf(tf, sizeof(tf), "tty=%d", tty);
  snprintf(df, sizeof(df), "detach=%d", detach);
  snprintf(of, sizeof(of), "overlay=%d", overlay);
  snprintf(nf, sizeof(nf), "name=%s", name ? name : "");
  snprintf(rowf, sizeof(rowf), "row=%d", row);
  snprintf(colf, sizeof(colf), "col=%d", col);

  char **fields = calloc(argc + 12, sizeof(char *));
  int nfld = 0;
  fields[nfld++] = "RUN";
  fields[nfld++] = rf;
  fields[nfld++] = tf;
  fields[nfld++] = df;
  fields[nfld++] = of;
  fields[nfld++] = nf;
  fields[nfld++] = nwf;
  fields[nfld++] = rowf;
  fields[nfld++] = colf;
  fields[nfld++] = "argv";
  for (int i = 0; i < argc; i++)
    fields[nfld++] = argv[i];

  if (send_fields(sock, fields, nfld) < 0) {
    close(sock);
    free(fields);
    return 1;
  }
  char *resp = read_response(sock);
  if (!resp) {
    close(sock);
    free(fields);
    return 1;
  }
  if (!strncmp(resp, "ERR ", 4)) {
    fprintf(stderr, "%s\n", resp + 4);
    free(resp);
    free(fields);
    close(sock);
    return 1;
  }
  char *id = strdup(resp + 3);
  free(resp);
  free(fields);

  if (detach) {
    printf("%s\n", id);
    close(sock);
    free(id);
    return 0;
  }

  int cfd = recv_fd(sock);
  close(sock);
  if (cfd < 0) {
    free(id);
    return 1;
  }

  interactive(cfd, tty);
  close(cfd);

  /* query exit code */
  sock = connect_daemon();
  if (sock >= 0) {
    char **wfields = (char *[]){"WAIT", id};
    send_fields(sock, wfields, 2);
    char *r = read_response(sock);
    close(sock);
    if (r && !strncmp(r, "OK ", 3)) {
      int code = atoi(r + 3);
      free(r);
      free(id);
      return code;
    }
    free(r);
  }
  free(id);
  return 0;
}

static int do_list(void) {
  int sock = connect_daemon();
  if (sock < 0)
    return 1;
  char *fields[] = {"LIST"};
  send_fields(sock, fields, 1);
  char *resp = read_response(sock);
  close(sock);
  if (!resp)
    return 1;
  if (!strncmp(resp, "ERR ", 4)) {
    fprintf(stderr, "%s\n", resp + 4);
    free(resp);
    return 1;
  }
  printf("%-16s %-8s %-10s %s\n", "ID", "PID", "STATUS", "COMMAND");
  printf("%s", resp + 3);
  free(resp);
  return 0;
}

static int do_kill(const char *id) {
  int sock = connect_daemon();
  if (sock < 0)
    return 1;
  char **fields = (char *[]){"KILL", (char *)id};
  send_fields(sock, fields, 2);
  char *resp = read_response(sock);
  close(sock);
  if (!resp)
    return 1;
  int ret = 0;
  if (!strncmp(resp, "ERR ", 4)) {
    fprintf(stderr, "%s\n", resp + 4);
    ret = 1;
  }
  free(resp);
  return ret;
}

static int do_logs(const char *id, int follow) {
  int sock = connect_daemon();
  if (sock < 0)
    return 1;
  char fbuf[16];
  snprintf(fbuf, sizeof(fbuf), "follow=%d", follow);
  char *fields[] = {"LOGS", (char *)id, fbuf};
  send_fields(sock, fields, 3);

  /* Read framed frames, then if follow, raw bytes */
  while (1) {
    uint32_t lb;
    if (read_full(sock, &lb, 4) != 4)
      break;
    uint32_t len = ntohl(lb);
    if (len == 0) {
      if (follow) {
        /* switch to raw streaming */
        char buf[4096];
        ssize_t r;
        while ((r = read(sock, buf, sizeof(buf))) > 0)
          fwrite(buf, 1, r, stdout);
      }
      break;
    }
    char *buf = malloc(len);
    if (read_full(sock, buf, len) != (ssize_t)len) {
      free(buf);
      break;
    }
    if (len > 4 && !memcmp(buf, "ERR ", 4)) {
      fprintf(stderr, "%.*s\n", (int)len - 4, buf + 4);
      free(buf);
      break;
    }
    fwrite(buf, 1, len, stdout);
    free(buf);
  }
  close(sock);
  return 0;
}

static int do_attach(const char *id) {
  int sock = connect_daemon();
  if (sock < 0)
    return 1;
  char *fields[] = {"ATTACH", (char *)id};
  send_fields(sock, fields, 2);
  char *resp = read_response(sock);
  if (!resp) {
    close(sock);
    return 1;
  }
  if (!strncmp(resp, "ERR ", 4)) {
    fprintf(stderr, "%s\n", resp + 4);
    free(resp);
    close(sock);
    return 1;
  }
  free(resp);
  int fd = recv_fd(sock);
  close(sock);
  if (fd < 0)
    return 1;
  interactive(fd, 1);
  close(fd);
  return 0;
}

/* ---------- main ---------- */
static void usage(const char *p) {
  fprintf(stderr,
          "usage:\n"
          "  %s run [--detach|-d] [--tty|-t] [--name NAME] [--overlay]\n"
          "        [--rootfs PATH] [--network container:<id>] <cmd> [args...]\n"
          "  %s ps | list\n"
          "  %s logs <id> [--follow|-f]\n"
          "  %s kill <id>\n"
          "  %s attach <id>\n"
          "  %s daemon      (start foreground)\n",
          p, p, p, p, p, p);
}

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);
  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }

  if (!strcmp(argv[1], "daemon")) {
    execlp("minidockerd", "minidockerd", (char *)NULL);
    perror("execlp minidockerd");
    return 1;
  }
  if (!strcmp(argv[1], "ps") || !strcmp(argv[1], "list"))
    return do_list();
  if (!strcmp(argv[1], "kill")) {
    if (argc < 3) {
      fprintf(stderr, "missing id\n");
      return 1;
    }
    return do_kill(argv[2]);
  }
  if (!strcmp(argv[1], "attach")) {
    if (argc < 3) {
      fprintf(stderr, "missing id\n");
      return 1;
    }
    return do_attach(argv[2]);
  }
  if (!strcmp(argv[1], "logs")) {
    if (argc < 3) {
      fprintf(stderr, "missing id\n");
      return 1;
    }
    int follow =
        (argc > 3 && (!strcmp(argv[3], "-f") || !strcmp(argv[3], "--follow")));
    return do_logs(argv[2], follow);
  }
  if (!strcmp(argv[1], "run")) {
    int detach = 0, tty = 0, overlay = 0;
    const char *name = NULL, *rootfs = "/tmp/minidocker/rootfs";
    const char *network = NULL;
    int i = 2;
    while (i < argc && !strncmp(argv[i], "-", 1)) {
      if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--detach"))
        detach = 1;
      else if (!strcmp(argv[i], "-t") || !strcmp(argv[i], "--tty"))
        tty = 1;
      else if (!strcmp(argv[i], "--overlay"))
        overlay = 1;
      else if (!strcmp(argv[i], "--name")) {
        if (i + 1 < argc)
          name = argv[++i];
      } else if (!strcmp(argv[i], "--rootfs")) {
        if (i + 1 < argc)
          rootfs = argv[++i];
      } else if (!strcmp(argv[i], "--network")) {
        if (i + 1 < argc)
          network = argv[++i];
      } else {
        fprintf(stderr, "unknown flag: %s\n", argv[i]);
        return 1;
      }
      i++;
    }
    if (i >= argc) {
      fprintf(stderr, "missing cmd\n");
      return 1;
    }
    return do_run(argc - i, &argv[i], tty, detach, rootfs, name, overlay,
                  network);
  }
  usage(argv[0]);
  return 1;
}