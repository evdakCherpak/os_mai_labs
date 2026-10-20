#include "morph.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "matrix.h"

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    size_t count;
    size_t waiting;
    unsigned long generation;
    int broken;
} barrier_t;

typedef enum {
    kErode,
    kDilate,
} morph_op_t;

// Полуинтервал строк [begin, end)
typedef struct {
    size_t begin;
    size_t end;
} row_range_t;

// границы включительно
typedef struct {
    size_t top;
    size_t bottom;
    size_t left;
    size_t right;
} window_t;

// сколько блоков строк приходится в среднем на поток при динамическом распределении
static const size_t kChunksPerThread = 16;

// Общие для всех потоков данные. Буферы результата каждого фильтра чередуются:
// итерация i пишет в out[i % 2] и читает то, что записала итерация i - 1.
// Так же чередуются счётчики блоков: next_chunk[i % 2] выдаёт блоки итерации i.
typedef struct {
    size_t rows;
    size_t cols;
    size_t radius;
    size_t iterations;
    int dynamic;
    size_t chunk_rows;
    size_t chunks;
    atomic_size_t next_chunk[2];
    const double* src;
    double* eroded[2];
    double* dilated[2];
    barrier_t barrier;
} morph_task_t;

// При статическом распределении поток обрабатывает полосу строк band.
typedef struct {
    morph_task_t* task;
    size_t index;
    row_range_t band;
    pthread_t thread;
} worker_t;

static int barrier_init(barrier_t* barrier, size_t count) {
    int err = pthread_mutex_init(&barrier->mutex, NULL);
    if (err != 0) {
        return err;
    }
    err = pthread_cond_init(&barrier->cond, NULL);
    if (err != 0) {
        pthread_mutex_destroy(&barrier->mutex);
        return err;
    }
    barrier->count = count;
    barrier->waiting = 0;
    barrier->generation = 0;
    barrier->broken = 0;
    return 0;
}

static void barrier_destroy(barrier_t* barrier) {
    pthread_cond_destroy(&barrier->cond);
    pthread_mutex_destroy(&barrier->mutex);
}

static int barrier_wait(barrier_t* barrier) {
    pthread_mutex_lock(&barrier->mutex);
    unsigned long generation = barrier->generation;
    if (!barrier->broken) {
        barrier->waiting++;
        if (barrier->waiting == barrier->count) {
            barrier->waiting = 0;
            barrier->generation++;
            pthread_cond_broadcast(&barrier->cond);
        } else {
            // Цикл защищает от ложных пробуждений pthread_cond_wait.
            while (generation == barrier->generation && !barrier->broken) {
                pthread_cond_wait(&barrier->cond, &barrier->mutex);
            }
        }
    }
    int passed = generation != barrier->generation;
    pthread_mutex_unlock(&barrier->mutex);
    return passed ? 0 : -1;
}

static void barrier_break(barrier_t* barrier) {
    pthread_mutex_lock(&barrier->mutex);
    barrier->broken = 1;
    pthread_cond_broadcast(&barrier->cond);
    pthread_mutex_unlock(&barrier->mutex);
}

static double window_min(const double* input, size_t cols, const window_t* window) {
    double result = input[(window->top * cols) + window->left];
    for (size_t i = window->top; i <= window->bottom; i++) {
        const double* row = input + (i * cols);
        for (size_t j = window->left; j <= window->right; j++) {
            if (row[j] < result) {
                result = row[j];
            }
        }
    }
    return result;
}

static double window_max(const double* input, size_t cols, const window_t* window) {
    double result = input[(window->top * cols) + window->left];
    for (size_t i = window->top; i <= window->bottom; i++) {
        const double* row = input + (i * cols);
        for (size_t j = window->left; j <= window->right; j++) {
            if (row[j] > result) {
                result = row[j];
            }
        }
    }
    return result;
}

// окно у края матрицы обрезается: учитываются только существующие элементы
static void filter_rows(
    const morph_task_t* task,
    morph_op_t operation,
    const double* input,
    double* output,
    row_range_t rows
) {
    size_t cols = task->cols;
    size_t radius = task->radius;
    for (size_t i = rows.begin; i < rows.end; i++) {
        window_t window = {
            .top = i >= radius ? i - radius : 0,
            .bottom = i + radius < task->rows ? i + radius : task->rows - 1,
        };
        for (size_t j = 0; j < cols; j++) {
            window.left = j >= radius ? j - radius : 0;
            window.right = j + radius < cols ? j + radius : cols - 1;
            if (operation == kErode) {
                output[(i * cols) + j] = window_min(input, cols, &window);
            } else {
                output[(i * cols) + j] = window_max(input, cols, &window);
            }
        }
    }
}

static void filter_both(const morph_task_t* task, size_t iteration, row_range_t rows) {
    const double* eroded_in = iteration == 0 ? task->src : task->eroded[(iteration - 1) % 2];
    const double* dilated_in = iteration == 0 ? task->src : task->dilated[(iteration - 1) % 2];
    filter_rows(task, kErode, eroded_in, task->eroded[iteration % 2], rows);
    filter_rows(task, kDilate, dilated_in, task->dilated[iteration % 2], rows);
}

static void run_iteration(const worker_t* worker, size_t iteration) {
    morph_task_t* task = worker->task;
    if (!task->dynamic) {
        filter_both(task, iteration, worker->band);
        return;
    }

    // Счётчик следующей итерации уже никто не трогает: все прошли барьер после предыдущей
    // Обнуление станет видно остальным через следующий барьер
    if (worker->index == 0) {
        atomic_store_explicit(&task->next_chunk[(iteration + 1) % 2], 0, memory_order_relaxed);
    }
    for (;;) {
        size_t chunk = atomic_fetch_add_explicit(&task->next_chunk[iteration % 2], 1, memory_order_relaxed);
        if (chunk >= task->chunks) {
            break;
        }
        row_range_t rows = {.begin = chunk * task->chunk_rows};
        rows.end = rows.begin + task->chunk_rows < task->rows ? rows.begin + task->chunk_rows : task->rows;
        filter_both(task, iteration, rows);
    }
}

static void* worker_main(void* arg) {
    const worker_t* worker = arg;
    morph_task_t* task = worker->task;

    if (barrier_wait(&task->barrier) != 0) {
        return NULL;
    }

    for (size_t i = 0; i < task->iterations; i++) {
        run_iteration(worker, i);
        if (i + 1 < task->iterations && barrier_wait(&task->barrier) != 0) {
            return NULL;
        }
    }
    return NULL;
}

static int run_workers(morph_task_t* task, size_t threads) {
    worker_t* workers = calloc(threads, sizeof(*workers));
    if (workers == NULL) {
        fprintf(stderr, "Not enough memory for %zu thread descriptors.\n", threads);
        return -1;
    }
    int err = barrier_init(&task->barrier, threads);
    if (err != 0) {
        fprintf(stderr, "Cannot initialize the barrier: %s\n", strerror(err));
        free(workers);
        return -1;
    }

    for (size_t i = 0; i < threads; i++) {
        workers[i].task = task;
        workers[i].index = i;
        workers[i].band.begin = i * task->rows / threads;
        workers[i].band.end = (i + 1) * task->rows / threads;
    }

    size_t started = 1;
    for (; started < threads; started++) {
        err = pthread_create(&workers[started].thread, NULL, worker_main, &workers[started]);
        if (err != 0) {
            fprintf(stderr, "Cannot create thread %zu of %zu: %s\n", started + 1, threads, strerror(err));
            barrier_break(&task->barrier);
            break;
        }
    }

    worker_main(&workers[0]);

    int result = err == 0 ? 0 : -1;
    for (size_t i = 1; i < started; i++) {
        err = pthread_join(workers[i].thread, NULL);
        if (err != 0) {
            fprintf(stderr, "Cannot join thread %zu: %s\n", i + 1, strerror(err));
            result = -1;
        }
    }

    barrier_destroy(&task->barrier);
    free(workers);
    return result;
}

int morph_apply(
    const matrix_t* src,
    matrix_t* eroded,
    matrix_t* dilated,
    const morph_params_t* params,
    size_t* threads_used
) {
    size_t total = src->rows * src->cols;
    *threads_used = 0;
    if (params->iterations == 0) {
        memcpy(eroded->data, src->data, total * sizeof(double));
        memcpy(dilated->data, src->data, total * sizeof(double));
        return 0;
    }

    size_t threads = params->max_threads < src->rows ? params->max_threads : src->rows;

    morph_task_t task = {
        .rows = src->rows,
        .cols = src->cols,
        .radius = params->window / 2,
        .iterations = params->iterations,
        .dynamic = !params->static_split,
        .src = src->data,
    };
    size_t target_chunks = threads * kChunksPerThread;
    task.chunk_rows = (src->rows + target_chunks - 1) / target_chunks;
    task.chunks = (src->rows + task.chunk_rows - 1) / task.chunk_rows;
    atomic_init(&task.next_chunk[0], 0);
    atomic_init(&task.next_chunk[1], 0);
    double* eroded_tmp = NULL;
    double* dilated_tmp = NULL;
    if (params->iterations > 1) {
        eroded_tmp = malloc(total * sizeof(double));
        dilated_tmp = malloc(total * sizeof(double));
        if (eroded_tmp == NULL || dilated_tmp == NULL) {
            fprintf(stderr, "Not enough memory for intermediate matrices.\n");
            free(eroded_tmp);
            free(dilated_tmp);
            return -1;
        }
    }
    size_t last = (params->iterations - 1) % 2;
    task.eroded[last] = eroded->data;
    task.eroded[1 - last] = eroded_tmp;
    task.dilated[last] = dilated->data;
    task.dilated[1 - last] = dilated_tmp;

    int result = run_workers(&task, threads);
    if (result == 0) {
        *threads_used = threads;
    }
    free(eroded_tmp);
    free(dilated_tmp);
    return result;
}
