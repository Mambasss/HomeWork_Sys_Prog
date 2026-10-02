#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <linux/sched.h>

#define STACK_SIZE (1024 * 1024)

char *rootfs_path;
char **container_argv;

int container_main(void *arg) {

    char sync_byte;
    if (read(*(int *)arg, &sync_byte, 1) != 1) {
        perror("read from pipe");
        return 1;
    }

    if (sethostname("container", 9) != 0) {
        perror("sethostname");
    }

    if (chroot(rootfs_path) != 0) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") != 0) {
        perror("chdir");
        return 1;
    }

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        if (mount(NULL, "/", NULL, MS_REC | MS_SLAVE, NULL) != 0) {
            fprintf(stderr, "Note: could not change / propagation (expected in rootless mode)\n");
        }
    }

    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount /proc");
        return 1;
    }

    if (mount("tmpfs", "/dev", "tmpfs", 0, "mode=755") != 0) {
        perror("mount /dev");
        return 1;
    }

    mknod("/dev/null", S_IFCHR | 0666, makedev(1, 3));
    mknod("/dev/zero", S_IFCHR | 0666, makedev(1, 5));
    mknod("/dev/random", S_IFCHR | 0666, makedev(1, 8));
    mknod("/dev/urandom", S_IFCHR | 0666, makedev(1, 9));

    execvp(container_argv[0], container_argv);
    perror("execvp");
    return 1;
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s run --rootfs <path> <command> [args...]\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "run") != 0 || strcmp(argv[2], "--rootfs") != 0) {
        fprintf(stderr, "Invalid arguments. Expected: run --rootfs <path> <command>...\n");
        return 1;
    }
    rootfs_path = argv[3];
    container_argv = &argv[4];

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        return 1;
    }

    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("malloc stack");
        return 1;
    }

    int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUSER | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET;

    pid_t child_pid = clone(container_main, stack + STACK_SIZE, flags | SIGCHLD, &pipefd[0]);
    if (child_pid == -1) {
        perror("clone");
        return 1;
    }

    char path[256], buf[256];
    snprintf(path, sizeof(path), "/proc/%d/uid_map", child_pid);

    int fd = open(path, O_WRONLY);
    if (fd == -1) {
        perror("open uid_map");
        return 1;
    }

    uid_t host_uid = getuid();
    snprintf(buf, sizeof(buf), "0 %d 1", host_uid);
    if (write(fd, buf, strlen(buf)) == -1) {
        perror("write uid_map");
        return 1;
    }
    close(fd);

    snprintf(path, sizeof(path), "/proc/%d/setgroups", child_pid);
    fd = open(path, O_WRONLY);
    if (fd != -1) {
        write(fd, "deny", 4);
        close(fd);
    }

    snprintf(path, sizeof(path), "/proc/%d/gid_map", child_pid);
    fd = open(path, O_WRONLY);
    if (fd == -1) {
        perror("open gid_map");
        return 1;
    }
    gid_t host_gid = getgid();
    snprintf(buf, sizeof(buf), "0 %d 1", host_gid);
    if (write(fd, buf, strlen(buf)) == -1) {
        perror("write gid_map");
        return 1;
    }
    close(fd);

    if (write(pipefd[1], "A", 1) != 1) {
        perror("write to pipe");
        return 1;
    }
    close(pipefd[1]);

    int status;
    waitpid(child_pid, &status, 0);

    free(stack);
    return WEXITSTATUS(status);
}