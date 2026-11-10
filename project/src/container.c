#include "container.h"
#include <net/if.h>
#include <netinet/in.h>

extern char **environ;

static void fail(const char *what) {
  perror(what);
  _exit(1);
}

static void bind_dev(const char *root, const char *dev) {
  char target[PATH_LEN], src[64];
  snprintf(target, sizeof(target), "%s/dev/%s", root, dev);
  snprintf(src, sizeof(src), "/dev/%s", dev);
  int fd = open(target, O_CREAT | O_RDONLY, 0666);
  if (fd >= 0)
    close(fd);
  mount(src, target, NULL, MS_BIND, NULL);
}

static int bring_up_loopback(void) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0)
    return -1;

  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);

  if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) {
    close(s);
    return -1;
  }
  ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
  if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) {
    close(s);
    return -1;
  }

  close(s);
  return 0;
}

static int child_fn(void *arg) {
  child_args_t *a = (child_args_t *)arg;

  /* 1. Wait for parent to install uid/gid maps */
  char b;
  if (read(a->sync_pipe, &b, 1) != 1)
    _exit(1);
  close(a->sync_pipe);

  if (a->netns_fd >= 0) {
    if (setns(a->netns_fd, CLONE_NEWNET) < 0)
      fail("setns(CLONE_NEWNET)");
    close(a->netns_fd);
  }

  /* 2. UTS */
  const char *hn = "minidocker";
  if (sethostname(hn, strlen(hn)) < 0)
    fail("sethostname");

  /* 3. Make host mounts non-propagating */
  if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
    fail("mount private");

  /* 4. OverlayFS (optional) */
  char *root = (char *)a->rootfs;
  if (a->overlay_upper) {
    char opts[PATH_LEN * 4];
    snprintf(opts, sizeof(opts), "lowerdir=%s,upperdir=%s,workdir=%s",
             a->rootfs, a->overlay_upper, a->overlay_work);
    if (mount("overlay", a->overlay_merged, "overlay", 0, opts) < 0)
      fail("overlay mount");
    root = (char *)a->overlay_merged;
  }

  /* 5. Bind-mount root to itself so pivot_root works */
  if (mount(root, root, NULL, MS_BIND | MS_REC, NULL) < 0)
    fail("bind root");

  /* 6. Bind essential /dev nodes from host (works rootless) */
  {
    char p[PATH_LEN];
    snprintf(p, sizeof(p), "%s/dev", root);
    mkdir(p, 0755);
  }
  const char *devs[] = {"null", "zero", "full", "random", "urandom", "tty"};
  for (size_t i = 0; i < sizeof(devs) / sizeof(devs[0]); i++)
    bind_dev(root, devs[i]);

  /* 7. pivot_root */
  if (chdir(root) < 0)
    fail("chdir root");
  if (syscall(SYS_pivot_root, ".", ".") < 0)
    fail("pivot_root");
  if (umount2(".", MNT_DETACH) < 0)
    fail("umount old root");
  if (chdir("/") < 0)
    fail("chdir /");

  /* 8. /proc */
  mkdir("/proc", 0555);

  int proc_mounted = 0;

  /* Уровень 1: свежий procfs с subset=pid (идеальный rootless-вариант) */
  if (mount("proc", "/proc", "proc", 0, "subset=pid") == 0) {
    proc_mounted = 1;
  }

  /* Уровень 2: обычный свежий procfs (может сработать, если /proc хоста
     не имеет locked child mounts) */
  if (!proc_mounted) {
    if (mount("proc", "/proc", "proc", 0, NULL) == 0) {
      proc_mounted = 1;
    }
  }

  /* Уровень 3: bind-mount существующего /proc хоста.
     Это не даёт изоляции PID, но позволяет контейнеру запуститься. */
  if (!proc_mounted) {
    if (mount("/proc", "/proc", NULL, MS_BIND | MS_REC, NULL) == 0) {
      proc_mounted = 1;
    }
  }

  /* Уровень 4: ничего не монтируем. /proc остаётся унаследованным
     от хоста (или пустой директорией). Контейнер всё равно запустится,
     хотя ps/top внутри работать не будут. Для базовых команд (echo,
     sh -c 'echo $$', exit) этого достаточно. */
  if (!proc_mounted) {
    fprintf(stderr, "warning: /proc is not mounted inside container "
                    "(rootless limitation); PID isolation is degraded\n");
  }

  /* 9. Поднять loopback в net namespace */
  if (a->netns_fd < 0) {
    if (bring_up_loopback() < 0)
      fprintf(stderr, "warning: bring_up_loopback: %s\n", strerror(errno));
  }

  /* 10. stdio */
  if (a->pty_slave >= 0) {
    setsid();
    ioctl(a->pty_slave, TIOCSCTTY, 0);
    dup2(a->pty_slave, 0);
    dup2(a->pty_slave, 1);
    dup2(a->pty_slave, 2);
    if (a->pty_slave > 2)
      close(a->pty_slave);
  } else if (a->stdout_pipe >= 0) {
    dup2(a->stdout_pipe, 1);
    dup2(a->stdout_pipe, 2);
    if (a->stdout_pipe > 2)
      close(a->stdout_pipe);
  }

  /* 11. exec */
  execvpe(a->argv[0], a->argv, environ);
  perror("execvpe");
  _exit(127);
}

pid_t container_spawn(child_args_t *args, int *sync_write_end, int rootless) {
  int sv[2];
  if (pipe(sv) < 0)
    return -1;
  args->sync_pipe = sv[0];

  int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC;
  /* CLONE_NEWNET - только если НЕ присоединяемся к чужой сети */
  if (args->netns_fd < 0)
    flags |= CLONE_NEWNET;
  if (rootless)
    flags |= CLONE_NEWUSER;

  static char child_stack[4 * 1024 * 1024];
  void *stack_top = child_stack + sizeof(child_stack);

  pid_t pid = clone(child_fn, stack_top, flags | SIGCHLD, args);
  if (pid < 0) {
    close(sv[0]);
    close(sv[1]);
    return -1;
  }

  close(sv[0]);
  *sync_write_end = sv[1];

  if (rootless) {
    char path[64], buf[64];
    int fd, n;

    snprintf(path, sizeof(path), "/proc/%d/setgroups", pid);
    fd = open(path, O_WRONLY);
    if (fd >= 0) {
      if (write(fd, "deny", 4) < 0) {
      }
      close(fd);
    }

    snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
      kill(pid, SIGKILL);
      close(sv[1]);
      return -1;
    }
    n = snprintf(buf, sizeof(buf), "0 %d 1\n", geteuid());
    if (write(fd, buf, n) < 0) {
      kill(pid, SIGKILL);
      close(fd);
      close(sv[1]);
      return -1;
    }
    close(fd);

    snprintf(path, sizeof(path), "/proc/%d/gid_map", pid);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
      kill(pid, SIGKILL);
      close(sv[1]);
      return -1;
    }
    n = snprintf(buf, sizeof(buf), "0 %d 1\n", getegid());
    if (write(fd, buf, n) < 0) {
      kill(pid, SIGKILL);
      close(fd);
      close(sv[1]);
      return -1;
    }
    close(fd);
  }

  return pid;
}