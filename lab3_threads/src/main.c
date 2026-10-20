#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "matrix.h"
#include "morph.h"

// Коды завершения программы.
enum {
    kExitUsage = 1,    // неверные ключи запуска
    kExitInput = 2,    // ошибка чтения или формата входной матрицы
    kExitCompute = 3,  // нехватка памяти, ошибка создания потоков
    kExitOutput = 4,   // ошибка открытия или записи выходного файла
};

static const size_t kMaxThreads = 4096;

typedef struct {
    morph_params_t morph;
    const char* input_path;
    const char* eroded_path;
    const char* dilated_path;
    size_t gen_rows;  // 0, если матрица читается, а не генерируется
    size_t gen_cols;
    uint64_t seed;
    int quiet;
    int verbose;
} options_t;

static void print_usage(const char* prog) {
    fprintf(
        stderr,
        "Usage: %s -t THREADS [-k K] [-w WINDOW] [-i FILE | -g ROWSxCOLS [-s SEED]]\n"
        "          [-e FILE] [-d FILE] [-S] [-q] [-v]\n"
        "Applies erosion and dilation K times to a matrix of real numbers.\n"
        "  -t THREADS  max number of threads working at the same time (1..%zu)\n"
        "  -k K        number of filter passes (default 1)\n"
        "  -w WINDOW   odd side of the square window (default 3)\n"
        "  -i FILE     read the matrix from FILE (default stdin): \"rows cols\" then values\n"
        "  -g RxC      generate a random R x C matrix instead of reading one\n"
        "  -s SEED     seed for -g (default 1)\n"
        "  -e FILE     write the eroded matrix to FILE (default stdout)\n"
        "  -d FILE     write the dilated matrix to FILE (default stdout)\n"
        "  -S          static split: one band of rows per thread (default: rows are\n"
        "              handed out to threads in small chunks as they finish)\n"
        "  -q          do not print results that have no output file\n"
        "  -v          print pid, used threads and filtering time to stderr\n",
        prog,
        kMaxThreads
    );
}

static int parse_size(const char* text, size_t* value, char** rest) {
    if (text[0] < '0' || text[0] > '9') {
        return -1;
    }
    char* end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno == ERANGE || parsed > SIZE_MAX || (rest == NULL && *end != '\0')) {
        return -1;
    }
    if (rest != NULL) {
        *rest = end;
    }
    *value = (size_t)parsed;
    return 0;
}

static int parse_option_size(char opt, const char* text, size_t min, size_t max, size_t* value) {
    if (parse_size(text, value, NULL) != 0 || *value < min || *value > max) {
        fprintf(stderr, "Option -%c: expected an integer in %zu..%zu, got '%s'.\n", opt, min, max, text);
        return -1;
    }
    return 0;
}

static int parse_shape(const char* text, size_t* rows, size_t* cols) {
    char* rest = NULL;
    if (parse_size(text, rows, &rest) != 0 || (*rest != 'x' && *rest != 'X') || parse_size(rest + 1, cols, NULL) != 0) {
        fprintf(stderr, "Option -g: expected ROWSxCOLS, got '%s'.\n", text);
        return -1;
    }
    return 0;
}

static int parse_options(int argc, char* argv[], options_t* options) {
    *options = (options_t){
        .morph = {.iterations = 1, .window = 3, .max_threads = 0},
        .seed = 1,
    };

    int opt = 0;
    size_t seed = 0;
    while ((opt = getopt(argc, argv, "t:k:w:i:g:s:e:d:Sqvh")) != -1) {
        int status = 0;
        switch (opt) {
            case 't':
                status = parse_option_size('t', optarg, 1, kMaxThreads, &options->morph.max_threads);
                break;
            case 'k':
                status = parse_option_size('k', optarg, 0, SIZE_MAX, &options->morph.iterations);
                break;
            case 'w':
                status = parse_option_size('w', optarg, 1, SIZE_MAX, &options->morph.window);
                if (status == 0 && options->morph.window % 2 == 0) {
                    fprintf(stderr, "Option -w: window side must be odd to have a center, got %s.\n", optarg);
                    status = -1;
                }
                break;
            case 'i':
                options->input_path = optarg;
                break;
            case 'g':
                status = parse_shape(optarg, &options->gen_rows, &options->gen_cols);
                break;
            case 's':
                status = parse_option_size('s', optarg, 0, SIZE_MAX, &seed);
                options->seed = seed;
                break;
            case 'e':
                options->eroded_path = optarg;
                break;
            case 'd':
                options->dilated_path = optarg;
                break;
            case 'S':
                options->morph.static_split = 1;
                break;
            case 'q':
                options->quiet = 1;
                break;
            case 'v':
                options->verbose = 1;
                break;
            case 'h':
                return 1;
            default:
                status = -1;
        }
        if (status != 0) {
            return -1;
        }
    }

    if (optind < argc) {
        fprintf(stderr, "Unexpected argument '%s'.\n", argv[optind]);
        return -1;
    }
    if (options->morph.max_threads == 0) {
        fprintf(stderr, "The maximum number of threads must be set with -t.\n");
        return -1;
    }
    if (options->input_path != NULL && options->gen_rows != 0) {
        fprintf(stderr, "Options -i and -g cannot be used together.\n");
        return -1;
    }
    return 0;
}

static int load_matrix(const options_t* options, matrix_t* matrix) {
    if (options->gen_rows != 0) {
        if (matrix_alloc(matrix, options->gen_rows, options->gen_cols) != 0) {
            return -1;
        }
        matrix_fill_random(matrix, options->seed);
        return 0;
    }

    if (options->input_path == NULL) {
        if (isatty(STDIN_FILENO)) {
            fprintf(stderr, "Enter the matrix: rows cols, then the values (Ctrl+D to finish):\n");
        }
        return matrix_read(stdin, matrix);
    }

    FILE* input = fopen(options->input_path, "r");
    if (input == NULL) {
        fprintf(stderr, "Cannot open '%s': %s\n", options->input_path, strerror(errno));
        return -1;
    }
    int result = matrix_read(input, matrix);
    fclose(input);
    return result;
}

// без имени файла результат идёт в stdout, а с -q не выводится (*out == NULL)
static int open_output(const char* path, int quiet, FILE** out) {
    *out = NULL;
    if (path == NULL) {
        if (!quiet) {
            *out = stdout;
        }
        return 0;
    }
    *out = fopen(path, "w");
    if (*out == NULL) {
        fprintf(stderr, "Cannot open '%s' for writing: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int close_output(FILE* out, const char* path) {
    if (out == NULL || out == stdout) {
        return 0;
    }
    if (fclose(out) != 0) {
        fprintf(stderr, "Cannot write '%s': %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int is_same_file(FILE* first, FILE* second) {
    if (first == NULL || second == NULL || first == stdout || second == stdout) {
        return 0;
    }
    struct stat first_stat;
    struct stat second_stat;
    if (fstat(fileno(first), &first_stat) != 0 || fstat(fileno(second), &second_stat) != 0) {
        perror("fstat");
        return 1;
    }
    if (first_stat.st_dev == second_stat.st_dev && first_stat.st_ino == second_stat.st_ino &&
        S_ISREG(first_stat.st_mode))
    {
        fprintf(stderr, "Erosion and dilation cannot be written to the same file.\n");
        return 1;
    }
    return 0;
}

static int write_output(FILE* out, const char* path, const matrix_t* matrix) {
    if (out != NULL && matrix_write(out, matrix) != 0) {
        fprintf(stderr, "Cannot write '%s': %s\n", path != NULL ? path : "stdout", strerror(errno));
        return -1;
    }
    return 0;
}

static double elapsed_ms(const struct timespec* start, const struct timespec* end) {
    return ((double)(end->tv_sec - start->tv_sec) * 1e3) + ((double)(end->tv_nsec - start->tv_nsec) / 1e6);
}

static int filter(const options_t* options, const matrix_t* src, matrix_t* eroded, matrix_t* dilated) {
    if (matrix_alloc(eroded, src->rows, src->cols) != 0 || matrix_alloc(dilated, src->rows, src->cols) != 0) {
        return -1;
    }
    if (options->verbose) {
        fprintf(stderr, "pid %d: filtering %zux%zu...\n", (int)getpid(), src->rows, src->cols);
    }

    struct timespec start;
    struct timespec end;
    size_t threads_used = 0;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int result = morph_apply(src, eroded, dilated, &options->morph, &threads_used);
    clock_gettime(CLOCK_MONOTONIC, &end);

    if (result == 0 && options->verbose) {
        fprintf(
            stderr,
            "matrix=%zux%zu window=%zu iterations=%zu threads=%zu split=%s time_ms=%.3f\n",
            src->rows,
            src->cols,
            options->morph.window,
            options->morph.iterations,
            threads_used,
            options->morph.static_split ? "static" : "dynamic",
            elapsed_ms(&start, &end)
        );
    }
    return result;
}

static int run(const options_t* options) {
    FILE* eroded_out = NULL;
    FILE* dilated_out = NULL;
    if (open_output(options->eroded_path, options->quiet, &eroded_out) != 0 ||
        open_output(options->dilated_path, options->quiet, &dilated_out) != 0 || is_same_file(eroded_out, dilated_out))
    {
        close_output(eroded_out, options->eroded_path);
        close_output(dilated_out, options->dilated_path);
        return kExitOutput;
    }

    matrix_t src = {0};
    matrix_t eroded = {0};
    matrix_t dilated = {0};
    int code = 0;
    if (load_matrix(options, &src) != 0) {
        code = kExitInput;
    } else if (filter(options, &src, &eroded, &dilated) != 0) {
        code = kExitCompute;
    } else if (
        write_output(eroded_out, options->eroded_path, &eroded) != 0 ||
        write_output(dilated_out, options->dilated_path, &dilated) != 0
    )
    {
        code = kExitOutput;
    }

    if (close_output(eroded_out, options->eroded_path) != 0) {
        code = kExitOutput;
    }
    if (close_output(dilated_out, options->dilated_path) != 0) {
        code = kExitOutput;
    }
    matrix_free(&src);
    matrix_free(&eroded);
    matrix_free(&dilated);
    return code;
}

int main(int argc, char* argv[]) {
    const char* prog = argc > 0 ? argv[0] : "morph";
    options_t options;
    int parsed = parse_options(argc, argv, &options);
    if (parsed != 0) {
        print_usage(prog);
        return parsed > 0 ? 0 : kExitUsage;
    }
    return run(&options);
}
