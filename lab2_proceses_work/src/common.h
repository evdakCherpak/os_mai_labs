#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>
#include <sys/types.h>

enum {
    kBlockSize = 4096,
};

// Коды завершения дочернего процесса, не дошедшего до main() программы slave.
enum {
    kChildSetupFailed = 126,
    kChildExecFailed = 127,
};

// Коды завершения программы slave.
enum {
    kSlaveReadFailed = 1,
    kSlaveWriteFailed = 2,
};

ssize_t read_retry(int fd, void* buf, size_t len);

int write_all(int fd, const void* buf, size_t len);

#endif
