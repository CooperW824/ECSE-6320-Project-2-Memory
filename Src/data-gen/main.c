/*
 * data-gen/main.c
 *
 * Usage: ./gen_floatdata <n_elements> <output_dir>
 *
 * Generates 6 binary files of n random values each in <output_dir>:
 *   a_f32.floatdata, x_f32.floatdata, y_f32.floatdata   (float, 4 bytes/elem)
 *   a_f64.floatdata, x_f64.floatdata, y_f64.floatdata   (double, 8 bytes/elem)
 *
 * Files are raw binary dumps of the native float/double representation
 * (host endianness, no header) — read them back with a plain fread()
 * into a float[]/double[] of the same length.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

typedef struct {
    const char *filename;
    int is_double; /* 0 = float32, 1 = float64 */
} FileSpec;

static const FileSpec kFiles[] = {
    { "a_f32.floatdata", 0 },
    { "x_f32.floatdata", 0 },
    { "y_f32.floatdata", 0 },
    { "a_f64.floatdata", 1 },
    { "x_f64.floatdata", 1 },
    { "y_f64.floatdata", 1 },
};
#define NUM_FILES (sizeof(kFiles) / sizeof(kFiles[0]))

/* Uniform random double in [-10.0, 10.0) */
static double rand_unit(void) {
    return ((double)rand() / ((double)RAND_MAX + 1.0)) * 20.0 - 10.0;
}

static int write_file(const char *outdir, const FileSpec *spec, size_t n) {
    char path[4096];
    int written = snprintf(path, sizeof(path), "%s/%s", outdir, spec->filename);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        fprintf(stderr, "path too long for %s\n", spec->filename);
        return -1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }

    size_t elem_size = spec->is_double ? sizeof(double) : sizeof(float);
    void *buf = malloc(elem_size * n);
    if (!buf) {
        fprintf(stderr, "malloc failed for %s (%zu elements)\n", spec->filename, n);
        fclose(f);
        return -1;
    }

    if (spec->is_double) {
        double *d = (double *)buf;
        for (size_t i = 0; i < n; i++) d[i] = rand_unit();
    } else {
        float *fl = (float *)buf;
        for (size_t i = 0; i < n; i++) fl[i] = (float)rand_unit();
    }

    size_t written_elems = fwrite(buf, elem_size, n, f);
    free(buf);
    fclose(f);

    if (written_elems != n) {
        fprintf(stderr, "short write for %s (%zu of %zu elements)\n",
                spec->filename, written_elems, n);
        return -1;
    }

    printf("wrote %s: %zu elements (%zu bytes)\n", path, n, n * elem_size);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <n_elements> <output_dir>\n", argv[0]);
        return 1;
    }

    char *endptr = NULL;
    errno = 0;
    unsigned long long n_ull = strtoull(argv[1], &endptr, 10);
    if (errno != 0 || endptr == argv[1] || *endptr != '\0' || n_ull == 0) {
        fprintf(stderr, "invalid element count: %s\n", argv[1]);
        return 1;
    }
    size_t n = (size_t)n_ull;
    const char *outdir = argv[2];

    srand((unsigned)time(NULL));

    for (size_t i = 0; i < NUM_FILES; i++) {
        if (write_file(outdir, &kFiles[i], n) != 0) {
            return 1;
        }
    }

    return 0;
}