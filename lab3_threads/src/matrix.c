#include "matrix.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    kTokenSize = 64,
};

// ограничение на одну сторону матрицы
static const size_t kMaxDimension = 1U << 20U;

int matrix_alloc(matrix_t* matrix, size_t rows, size_t cols) {
    matrix->rows = 0;
    matrix->cols = 0;
    matrix->data = NULL;
    if (rows == 0 || cols == 0 || rows > kMaxDimension || cols > kMaxDimension) {
        fprintf(stderr, "Invalid matrix size %zux%zu (each side must be 1..%zu).\n", rows, cols, kMaxDimension);
        return -1;
    }
    if (rows > SIZE_MAX / sizeof(double) / cols) {
        fprintf(stderr, "Matrix %zux%zu does not fit into the address space.\n", rows, cols);
        return -1;
    }

    matrix->data = malloc(rows * cols * sizeof(double));
    if (matrix->data == NULL) {
        fprintf(stderr, "Not enough memory for a %zux%zu matrix.\n", rows, cols);
        return -1;
    }
    matrix->rows = rows;
    matrix->cols = cols;
    return 0;
}

void matrix_free(matrix_t* matrix) {
    free(matrix->data);
    matrix->data = NULL;
    matrix->rows = 0;
    matrix->cols = 0;
}

static int read_token(FILE* input, char token[kTokenSize]) {
    int scanned = fscanf(input, "%63s", token);
    if (scanned == 1) {
        return 1;
    }
    if (ferror(input)) {
        perror("Failed to read the matrix");
        return -1;
    }
    return 0;
}

static int read_dimension(FILE* input, const char* name, size_t* value) {
    char token[kTokenSize];
    int status = read_token(input, token);
    if (status <= 0) {
        if (status == 0) {
            fprintf(stderr, "Matrix header: missing %s.\n", name);
        }
        return -1;
    }

    char* end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(token, &end, 10);
    if (token[0] == '-' || token[0] == '+' || end == token || *end != '\0' || errno == ERANGE || parsed > SIZE_MAX) {
        fprintf(stderr, "Matrix header: invalid %s '%s'.\n", name, token);
        return -1;
    }
    *value = (size_t)parsed;
    return 0;
}

int matrix_read(FILE* input, matrix_t* matrix) {
    size_t rows = 0;
    size_t cols = 0;
    if (read_dimension(input, "row count", &rows) != 0 || read_dimension(input, "column count", &cols) != 0) {
        return -1;
    }
    if (matrix_alloc(matrix, rows, cols) != 0) {
        return -1;
    }

    size_t total = rows * cols;
    char token[kTokenSize];
    for (size_t i = 0; i < total; i++) {
        int status = read_token(input, token);
        if (status <= 0) {
            if (status == 0) {
                fprintf(stderr, "Matrix data: expected %zu numbers, got %zu.\n", total, i);
            }
            matrix_free(matrix);
            return -1;
        }

        char* end = NULL;
        double value = strtod(token, &end);
        if (end == token || *end != '\0' || isnan(value)) {
            fprintf(stderr, "Matrix data: invalid number '%s' at row %zu, column %zu.\n", token, i / cols, i % cols);
            matrix_free(matrix);
            return -1;
        }
        matrix->data[i] = value;
    }

    int status = read_token(input, token);
    if (status != 0) {
        if (status == 1) {
            fprintf(stderr, "Matrix data: unexpected extra value '%s'.\n", token);
        }
        matrix_free(matrix);
        return -1;
    }
    return 0;
}

int matrix_write(FILE* out, const matrix_t* matrix) {
    if (fprintf(out, "%zu %zu\n", matrix->rows, matrix->cols) < 0) {
        return -1;
    }
    for (size_t i = 0; i < matrix->rows; i++) {
        const double* row = matrix->data + (i * matrix->cols);
        for (size_t j = 0; j < matrix->cols; j++) {
            // 15 значащих цифр восстанавливают любое десятичное число, введённое с не большей точностью.
            if (fprintf(out, j + 1 < matrix->cols ? "%.15g " : "%.15g\n", row[j]) < 0) {
                return -1;
            }
        }
    }
    return fflush(out) == 0 ? 0 : -1;
}

// splitmix64 - воспроизводимый по сиду
static uint64_t next_random(uint64_t* state) {
    uint64_t mixed = (*state += 0x9E3779B97F4A7C15ULL);
    mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
    return mixed ^ (mixed >> 31U);
}

void matrix_fill_random(matrix_t* matrix, uint64_t seed) {
    uint64_t state = seed;
    size_t total = matrix->rows * matrix->cols;
    for (size_t i = 0; i < total; i++) {
        matrix->data[i] = (double)(next_random(&state) % 10000U) / 100.0;
    }
}
