#ifndef MORPH_H
#define MORPH_H

#include <stddef.h>

#include "matrix.h"

typedef struct {
    size_t iterations;
    size_t window;       // сторона квадратного окна, нечётная
    size_t max_threads;  // максимум одновременно работающих потоков, включая главный
    int static_split;    // 1: каждому потоку одна полоса строк, 0: блоки строк раздаются по мере готовности
} morph_params_t;

// В *threads_used записывается число потоков, реально выполнявших вычисления
int morph_apply(
    const matrix_t* src,
    matrix_t* eroded,
    matrix_t* dilated,
    const morph_params_t* params,
    size_t* threads_used
);

#endif
