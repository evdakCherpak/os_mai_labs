#include "common.h"

#include <errno.h>
#include <stddef.h>
#include <unistd.h>

ssize_t read_retry(int fd, void* buf, size_t len) {
    ssize_t bytes_read = 0;
    do {
        bytes_read = read(fd, buf, len);
    } while (bytes_read == -1 && errno == EINTR);
    return bytes_read;
}

int write_all(int fd, const void* buf, size_t len) {
    const char* cursor = buf;
    while (len > 0) {
        ssize_t written = write(fd, cursor, len);
        if (written == -1) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        cursor += written;
        len -= (size_t)written;
    }
    return 0;
}
