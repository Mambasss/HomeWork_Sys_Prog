#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <linux/sched.h>

#define STACK_SIZE (1024 * 1024)
#define BUF_SIZE   8192

static char *rootfs_path;
static char **container_argv;
static int   use_pty    = 0;
static int   slave_fd   = -1;
static int   master_fd  = -1;
static struct termios orig_termios;
static pid_t child_pid  = -1;

static void restore_terminal(void) {
    if (use_pty && isatty(STDIN_FILENO))
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    if (master_fd != -1) { close(master_fd); master_fd = -1; }
}

static void sig_handler(int sig) {
    restore_terminal();
    if (child_pid > 0) kill(child_pid, SIGTERM);
    _exit(128 + sig);
}

static void relay_loop(int master) {
    struct pollfd fds[2];
    char buf[BUF_SIZE];
    fds[0].fd = STDIN_FILENO; fds[0].events = POLLIN;
    fds[1].fd = master;       fds[1].events = POLLIN;

    for (;;) {
        int n = poll(fds, 2, -1);
        if (n == -1) { if (errno == EINTR) continue; break; }

        if (fds[0].revents & POLLIN) {
            ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
            if (r <= 0) break;
            if (write(master, buf, (size_t)r) != r) break;
        }
        if (fds[1].revents & POLLIN) {
            ssize_t r = read(master, buf, sizeof(buf));
            if (r <= 0) break;
            if (write(STDOUT_FILENO, buf, (size_t)r) != r) break;
        }
        if (fds[1].revents & (POLLHUP | POLLERR)) break;
    }
}

int container_main(void *arg) {
    int sync_fd = *(int *)arg;
    char sync_byte;
    if (read(sync_fd, &sync_byte, 1) != 1) { perror("read pipe"); return 1; }
    close(sync_fd);

    if (sethostname("container", 9) != 0)
        perror("sethostname");

    if (chroot(rootfs_path) != 0) { perror("chroot"); return 1; }
    if (chdir("/") != 0)          { perror("chdir");  return 1; }

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        if (mount(NULL, "/", NULL, MS_REC | MS_SLAVE, NULL) != 0)
            fprintf(stderr, "Note: could not change / propagation\n");
    }

    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount /proc"); return 1;
    }
    if (mount("tmpfs", "/dev", "tmpfs", 0, "mode=755") != 0) {
        perror("mount /dev"); return 1;
    }
    mknod("/dev/null",    S_IFCHR | 0666, makedev(1, 3));
    mknod("/dev/zero",    S_IFCHR | 0666, makedev(1, 5));
    mknod("/dev/random",  S_IFCHR | 0666, makedev(1, 8));
    mknod("/dev/urandom", S_IFCHR | 0666, makedev(1, 9));

    if (use_pty) {
        if (setsid() == -1) { perror("setsid"); return 1; }
        if (ioctl(slave_fd, TIOCSCTTY, 0) == -1) {
            perror("ioctl TIOCSCTTY"); return 1;
        }
        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        if (slave_fd > STDERR_FILENO) close(slave_fd);
    }

    execvp(container_argv[0], container_argv);
    perror("execvp");
    return 1;
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr,
            "Usage: %s run --rootfs <path> [-it] <command> [args...]\n", argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "run") != 0 || strcmp(argv[2], "--rootfs") != 0) {
        fprintf(stderr, "Expected: run --rootfs <path> [-it] <command>...\n");
        return 1;
    }
    rootfs_path = argv[3];

    int cmd_start = 4;
    while (cmd_start < argc) {
        if      (!strcmp(argv[cmd_start], "-it") ||
                 !strcmp(argv[cmd_start], "-ti") ||
                 !strcmp(argv[cmd_start], "-t")) { use_pty = 1; cmd_start++; }
        else if (!strcmp(argv[cmd_start], "-i"))  { cmd_start++; }
        else break;
    }
    if (cmd_start >= argc) { fprintf(stderr, "no command\n"); return 1; }
    container_argv = &argv[cmd_start];

    if (use_pty) {
        if (!isatty(STDIN_FILENO)) {
            fprintf(stderr, "-it requires a TTY on stdin\n");
            return 1;
        }
        master_fd = posix_openpt(O_RDWR | O_NOCTTY);
        if (master_fd == -1) { perror("posix_openpt"); return 1; }
        if (grantpt(master_fd) == -1 || unlockpt(master_fd) == -1) {
            perror("grantpt/unlockpt"); return 1;
        }
        const char *slave_name = ptsname(master_fd);
        if (!slave_name) { perror("ptsname"); return 1; }
        slave_fd = open(slave_name, O_RDWR | O_NOCTTY);
        if (slave_fd == -1) { perror("open slave"); return 1; }

        if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
            perror("tcgetattr"); return 1;
        }
        atexit(restore_terminal);
        signal(SIGINT,  sig_handler);
        signal(SIGTERM, sig_handler);
        signal(SIGHUP,  sig_handler);
    }

    int pipefd[2];
    if (pipe(pipefd) == -1) { perror("pipe"); return 1; }

    char *stack = malloc(STACK_SIZE);
    if (!stack) { perror("malloc"); return 1; }

    int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUSER
              | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET;

    pid_t pid = clone(container_main, stack + STACK_SIZE, flags | SIGCHLD, &pipefd[0]);
    if (pid == -1) { perror("clone"); return 1; }
    child_pid = pid;

    if (use_pty && slave_fd != -1) { close(slave_fd); slave_fd = -1; }

    char path[256], buf[256];
    snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);
    int fd = open(path, O_WRONLY);
    if (fd == -1) { perror("open uid_map"); return 1; }
    snprintf(buf, sizeof(buf), "0 %d 1", getuid());
    if (write(fd, buf, strlen(buf)) == -1) { perror("write uid_map"); return 1; }
    close(fd);

    snprintf(path, sizeof(path), "/proc/%d/setgroups", pid);
    fd = open(path, O_WRONLY);
    if (fd != -1) { write(fd, "deny", 4); close(fd); }

    snprintf(path, sizeof(path), "/proc/%d/gid_map", pid);
    fd = open(path, O_WRONLY);
    if (fd == -1) { perror("open gid_map"); return 1; }
    snprintf(buf, sizeof(buf), "0 %d 1", getgid());
    if (write(fd, buf, strlen(buf)) == -1) { perror("write gid_map"); return 1; }
    close(fd);

    if (write(pipefd[1], "A", 1) != 1) { perror("write pipe"); return 1; }
    close(pipefd[1]);

    if (use_pty) {
        struct termios raw = orig_termios;
        cfmakeraw(&raw);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == -1)
            perror("tcsetattr raw");

        relay_loop(master_fd);
    }

    int status;
    waitpid(pid, &status, 0);

    if (use_pty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
        if (master_fd != -1) { close(master_fd); master_fd = -1; }
    }

    free(stack);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}