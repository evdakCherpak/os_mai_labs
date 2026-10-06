#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define SLAVE_NAME "slave"

enum { kChildrenCount = 2 };

// Биты кода завершения, могут сочетаться, 0 означает успех
static const unsigned int kErrInput = 0x01U;         // ошибка чтения stdin или имени файла
static const unsigned int kErrSetup = 0x02U;         // ошибка open/pipe/fork/sigaction
static const unsigned int kErrChild1Write = 0x04U;   // child1 перестал читать pipe1
static const unsigned int kErrChild2Write = 0x08U;   // child2 перестал читать pipe2
static const unsigned int kErrChild1Status = 0x10U;  // child1 завершился с ошибкой
static const unsigned int kErrChild2Status = 0x20U;  // child2 завершился с ошибкой

typedef struct {
    const char* label;
    unsigned int write_err;
    unsigned int status_err;
    char filename[PATH_MAX];
    int file_fd;
    int pipe_fd[2];
    pid_t pid;
} child_t;

static int check_std_fds(void) {
    int all_open = 1;
    for (int fd = 0; fd <= 2; fd++) {
        if (fcntl(fd, F_GETFD) == -1) {
            fprintf(stderr, "Standard descriptor %d is closed.\n", fd);
            all_open = 0;
        }
    }
    if (!all_open) {
        return 1;
    }
    return 0;
}

static void close_fd(int* fd) {
    if (*fd != -1) {
        close(*fd);
        *fd = -1;
    }
}

static int set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags == -1) {
        return -1;
    }
    return fcntl(fd, F_SETFD, (int)((unsigned int)flags | (unsigned int)FD_CLOEXEC));
}

static int open_pipe(int fds[2]) {
    if (pipe(fds) == -1) {
        perror("pipe");
        return -1;
    }
    if (set_cloexec(fds[0]) == -1 || set_cloexec(fds[1]) == -1) {
        perror("fcntl");
        return -1;
    }
    return 0;
}

static int build_slave_path(const char* argv0, char* path, size_t size) {
    const char* dir = "./";
    int dir_len = 2;
    if (argv0 != NULL) {
        const char* slash = strrchr(argv0, '/');
        if (slash != NULL) {
            dir = argv0;
            dir_len = (int)(slash - argv0 + 1);
        }
    }

    int len = snprintf(path, size, "%.*s%s", dir_len, dir, SLAVE_NAME);
    if (len < 0 || (size_t)len >= size) {
        fprintf(stderr, "Path to the slave program is too long.\n");
        return -1;
    }
    return 0;
}

static int read_filename(const char* label, char* name, size_t size) {
    if (isatty(STDIN_FILENO)) {
        fprintf(stderr, "Enter file name for %s: ", label);
    }

    size_t len = 0;
    for (;;) {
        char symbol = '\0';
        ssize_t bytes_read = read_retry(STDIN_FILENO, &symbol, 1);
        if (bytes_read == -1) {
            perror("Failed to read input");
            return -1;
        }
        if (bytes_read == 0 || symbol == '\n') {
            break;
        }
        if (len + 1 >= size) {
            fprintf(stderr, "File name for %s is too long.\n", label);
            return -1;
        }
        name[len++] = symbol;
    }

    if (len > 0 && name[len - 1] == '\r') {
        len--;
    }
    name[len] = '\0';

    if (len == 0) {
        fprintf(stderr, "No file name for %s.\n", label);
        return -1;
    }
    return 0;
}

static int open_output_file(const char* filename) {
    int fd = -1;
    do {
        fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    } while (fd == -1 && errno == EINTR);

    if (fd == -1) {
        fprintf(stderr, "Cannot open '%s' for writing: %s\n", filename, strerror(errno));
    }
    return fd;
}

static int is_same_regular_file(int fd1, int fd2) {
    struct stat first;
    struct stat second;
    if (fstat(fd1, &first) == -1 || fstat(fd2, &second) == -1) {
        perror("fstat");
        return -1;
    }
    return S_ISREG(first.st_mode) && first.st_dev == second.st_dev && first.st_ino == second.st_ino;
}

static pid_t spawn_slave(const char* slave_path, int in_fd, int out_fd) {
    pid_t pid = fork();
    if (pid != 0) {
        return pid;
    }

    if (dup2(in_fd, STDIN_FILENO) == -1 || dup2(out_fd, STDOUT_FILENO) == -1) {
        perror("dup2");
        _exit(kChildSetupFailed);
    }

    char* const kArgs[] = {(char*)SLAVE_NAME, NULL};
    execv(slave_path, kArgs);

    fprintf(stderr, "Cannot execute '%s': %s\n", slave_path, strerror(errno));
    _exit(kChildExecFailed);
}

static int wait_child(const child_t* child) {
    int status = 0;
    pid_t waited = 0;
    do {
        waited = waitpid(child->pid, &status, 0);
    } while (waited == -1 && errno == EINTR);

    if (waited == -1) {
        fprintf(stderr, "waitpid for %s failed: %s\n", child->label, strerror(errno));
        return 1;
    }

    if (WIFSIGNALED(status)) {
        fprintf(stderr, "%s was killed by signal %d.\n", child->label, WTERMSIG(status));
        return 1;
    }

    int code = WEXITSTATUS(status);
    switch (code) {
        case 0:
            return 0;
        case kChildExecFailed:
            fprintf(stderr, "%s could not execute the slave program.\n", child->label);
            break;
        case kChildSetupFailed:
            fprintf(stderr, "%s failed to set up its descriptors.\n", child->label);
            break;
        case kSlaveReadFailed:
            fprintf(stderr, "%s failed to read its pipe.\n", child->label);
            break;
        case kSlaveWriteFailed:
            fprintf(stderr, "%s failed to write its file.\n", child->label);
            break;
        default:
            fprintf(stderr, "%s finished with exit code %d.\n", child->label, code);
    }
    return 1;
}

static unsigned int start_children(child_t children[kChildrenCount], const char* argv0) {
    char slave_path[PATH_MAX];
    if (build_slave_path(argv0, slave_path, sizeof(slave_path)) != 0) {
        return kErrSetup;
    }

    for (int i = 0; i < kChildrenCount; i++) {
        if (read_filename(children[i].label, children[i].filename, sizeof(children[i].filename)) != 0) {
            return kErrInput;
        }
    }

    for (int i = 0; i < kChildrenCount; i++) {
        children[i].file_fd = open_output_file(children[i].filename);
        if (children[i].file_fd == -1) {
            return kErrSetup;
        }
    }

    int same = is_same_regular_file(children[0].file_fd, children[1].file_fd);
    if (same == 1) {
        fprintf(stderr, "Both children cannot write to the same file.\n");
        return kErrInput;
    }
    if (same == -1) {
        return kErrSetup;
    }

    for (int i = 0; i < kChildrenCount; i++) {
        if (open_pipe(children[i].pipe_fd) != 0) {
            return kErrSetup;
        }
    }

    for (int i = 0; i < kChildrenCount; i++) {
        children[i].pid = spawn_slave(slave_path, children[i].pipe_fd[0], children[i].file_fd);
        if (children[i].pid == -1) {
            fprintf(stderr, "fork for %s failed: %s\n", children[i].label, strerror(errno));
            return kErrSetup;
        }
        close_fd(&children[i].pipe_fd[0]);
        close_fd(&children[i].file_fd);
    }

    struct sigaction action = {0};
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGPIPE, &action, NULL) == -1) {
        perror("sigaction");
        return kErrSetup;
    }
    return 0;
}

static unsigned int forward_lines(child_t children[kChildrenCount]) {
    if (isatty(STDIN_FILENO)) {
        fprintf(stderr, "Enter lines (Ctrl+D to finish):\n");
    }

    char buf[kBlockSize];
    size_t line_index = 0;
    int alive = kChildrenCount;
    unsigned int flags = 0;

    while (alive > 0) {
        ssize_t bytes_read = read_retry(STDIN_FILENO, buf, sizeof(buf));
        if (bytes_read == 0) {
            break;
        }
        if (bytes_read == -1) {
            perror("Failed to read input");
            flags |= kErrInput;
            break;
        }

        char* start = buf;
        char* end = buf + bytes_read;
        while (start < end) {
            char* newline = memchr(start, '\n', (size_t)(end - start));
            char* piece_end = end;
            child_t* dst = &children[line_index % kChildrenCount];
            if (newline != NULL) {
                piece_end = newline + 1;
                line_index++;
            }

            int* fd = &dst->pipe_fd[1];
            if (*fd != -1 && write_all(*fd, start, (size_t)(piece_end - start))) {
                if (errno == EPIPE) {
                    fprintf(stderr, "%s stopped reading; its next lines are dropped.\n", dst->label);
                } else {
                    fprintf(stderr, "Write to %s failed: %s\n", dst->label, strerror(errno));
                }
                close_fd(fd);
                alive--;
                flags |= dst->write_err;
            }
            start = piece_end;
        }
    }
    return flags;
}

static unsigned int finish_children(child_t children[kChildrenCount]) {
    for (int i = 0; i < kChildrenCount; i++) {
        close_fd(&children[i].pipe_fd[0]);
        close_fd(&children[i].pipe_fd[1]);
        close_fd(&children[i].file_fd);
    }

    unsigned int flags = 0;
    for (int i = 0; i < kChildrenCount; i++) {
        if (children[i].pid > 0 && wait_child(&children[i]) != 0) {
            flags |= children[i].status_err;
        }
    }
    return flags;
}

int main(int argc, char* argv[]) {
    if (check_std_fds() != 0) {
        return (int)kErrSetup;
    }

    child_t children[kChildrenCount] = {
        {
            .label = "child1",
            .write_err = kErrChild1Write,
            .status_err = kErrChild1Status,
            .file_fd = -1,
            .pipe_fd = {-1, -1},
            .pid = -1,
        },
        {
            .label = "child2",
            .write_err = kErrChild2Write,
            .status_err = kErrChild2Status,
            .file_fd = -1,
            .pipe_fd = {-1, -1},
            .pid = -1,
        },
    };

    const char* argv0 = NULL;
    if (argc > 0) {
        argv0 = argv[0];
    }
    unsigned int result = start_children(children, argv0);
    if (result == 0) {
        result = forward_lines(children);
    }
    return (int)(result | finish_children(children));
}
