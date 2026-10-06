#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "common.h"

static int is_vowel(char symbol) {
    return symbol != '\0' && strchr("aeiouAEIOU", symbol) != NULL;
}

int main(void) {
    char buf[kBlockSize];
    ssize_t bytes_read = 0;

    while ((bytes_read = read_retry(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        size_t len = 0;
        for (ssize_t i = 0; i < bytes_read; i++) {
            if (!is_vowel(buf[i])) {
                buf[len++] = buf[i];
            }
        }

        if (write_all(STDOUT_FILENO, buf, len) != 0) {
            fprintf(stderr, "slave[%d]: write failed: %s\n", (int)getpid(), strerror(errno));
            return kSlaveWriteFailed;
        }
    }

    if (bytes_read == -1) {
        fprintf(stderr, "slave[%d]: read failed: %s\n", (int)getpid(), strerror(errno));
        return kSlaveReadFailed;
    }
    return 0;
}
