#ifndef MATRIX_H
#define MATRIX_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// элемент (r, c) лежит в data[r * cols + c]
typedef struct {
    size_t rows;
    size_t cols;
    double* data;
} matrix_t;

int matrix_alloc(matrix_t* matrix, size_t rows, size_t cols);

void matrix_free(matrix_t* matrix);

// Формат: "rows cols", затем rows * cols чисел через пробелы
int matrix_read(FILE* input, matrix_t* matrix);

int matrix_write(FILE* out, const matrix_t* matrix);

void matrix_fill_random(matrix_t* matrix, uint64_t seed);

#endif
