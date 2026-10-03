#define _POSIX_C_SOURCE 200809L
#include "inc/kernels.h"
#include "inc/perf.h"

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
  kernel_task task;
  size_t begin, end;
} worker_arg;

static void usage(const char *program) {
  fprintf(stderr,
          "Usage: %s --n ELEMENTS [--a FILE] [--x FILE] [--y FILE]\n"
          "          [--threads N] [--stride N] [--pattern stride|random]\n"
          "          [--fill VALUE] [--scalar VALUE] [--seed N]\n"
          "  --n is the number of logical elements; the last accessed input "
          "index is\n"
          "  (n - 1) * stride. Random mode shuffles those strided positions.\n",
          program);
}

static int parse_size(const char *s, size_t *value) {
  char *end = NULL;
  errno = 0;
  unsigned long long n = strtoull(s, &end, 10);
  if (errno || end == s || *end || n == 0 || n > SIZE_MAX)
    return -1;
  *value = (size_t)n;
  return 0;
}

static int parse_value(const char *s, kernel_element *value) {
  char *end = NULL;
  errno = 0;
  double n = strtod(s, &end);
  if (errno || end == s || *end)
    return -1;
  *value = (kernel_element)n;
  return 0;
}

static kernel_element *load_data(const char *path, size_t required,
                                 const char *label) {
  if (!path)
    return NULL;
  FILE *file = fopen(path, "rb");
  if (!file) {
    fprintf(stderr, "cannot open %s (%s): %s\n", label, path, strerror(errno));
    return NULL;
  }
  struct stat st;
  if (fstat(fileno(file), &st) != 0 || st.st_size < 0 ||
      (uintmax_t)st.st_size % sizeof(kernel_element) != 0) {
    fprintf(stderr, "invalid data file size: %s\n", path);
    fclose(file);
    return NULL;
  }
  size_t count = (size_t)st.st_size / sizeof(kernel_element);
  if (count < required) {
    fprintf(stderr, "%s contains %zu elements; need at least %zu\n", path,
            count, required);
    fclose(file);
    return NULL;
  }
  kernel_element *data = malloc((count ? count : 1) * sizeof(*data));
  if (!data || fread(data, sizeof(*data), count, file) != count) {
    fprintf(stderr, "failed to read %s\n", path);
    free(data);
    fclose(file);
    return NULL;
  }
  fclose(file);
  return data;
}

static void *run_worker(void *opaque) {
  worker_arg *arg = opaque;
  kernel_RunRange(&arg->task, arg->begin, arg->end);
  return NULL;
}

/**
 * @brief Copies tightly packed input elements into strided positions in output.
 *
 * @param input        Array of num_elements floats/doubles, tightly packed.
 * @param output       Buffer of at least
 *                     start_offset + ((num_elements - 1) * stride + 1) *
 *                     element_size bytes.
 * @param num_elements Number of floats/doubles in `input`.
 * @param element_size Size of an element in bytes.
 * @param start_offset Number of leading bytes in `output` to leave as zero.
 * @param stride       Spacing between successive elements, in units of
 *                     element_size.
 */
void format_input_data(const char *input, char *output, uint32_t num_elements,
                       size_t element_size, size_t start_offset,
                       size_t stride) {
  if (num_elements == 0)
    return;
  size_t total_size = start_offset +
                      ((size_t)(num_elements - 1) * stride + 1) * element_size;

  /* Zero everything first — this covers the leading start_offset bytes
   * as well as any padding gaps introduced by stride > 1. */
  memset(output, 0, total_size);

  for (uint32_t i = 0; i < num_elements; ++i) {
    const char *src = input + (size_t)i * element_size;
    char *dst = output + start_offset + (size_t)i * stride * element_size;
    memcpy(dst, src, element_size);
  }
}

int main(int argc, char **argv) {
  const char *path_a = NULL, *path_x = NULL, *path_y = NULL;
  size_t n = 0, threads = 1, stride = 1, seed = 1;
  kernel_element fill = 0, scalar = 1;
  int random_order = 0, have_n = 0;
  static const struct option options[] = {
      {"n", required_argument, NULL, 'n'},
      {"a", required_argument, NULL, 'a'},
      {"x", required_argument, NULL, 'x'},
      {"y", required_argument, NULL, 'y'},
      {"threads", required_argument, NULL, 't'},
      {"stride", required_argument, NULL, 'd'},
      {"pattern", required_argument, NULL, 'p'},
      {"fill", required_argument, NULL, 'f'},
      {"scalar", required_argument, NULL, 's'},
      {"seed", required_argument, NULL, 'r'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0}};
  int option;
  while ((option = getopt_long(argc, argv, "n:a:x:y:t:d:p:f:s:r:h", options,
                               NULL)) != -1) {
    size_t *target = NULL;
    switch (option) {
    case 'n':
      target = &n;
      have_n = 1;
      break;
    case 't':
      target = &threads;
      break;
    case 'd':
      target = &stride;
      break;
    case 'r':
      target = &seed;
      break;
    case 'a':
      path_a = optarg;
      continue;
    case 'x':
      path_x = optarg;
      continue;
    case 'y':
      path_y = optarg;
      continue;
    case 'p':
      if (!strcmp(optarg, "stride"))
        random_order = 0;
      else if (!strcmp(optarg, "random"))
        random_order = 1;
      else {
        fprintf(stderr, "pattern must be stride or random\n");
        return 2;
      }
      continue;
    case 'f':
      if (parse_value(optarg, &fill)) {
        fprintf(stderr, "invalid fill value\n");
        return 2;
      }
      continue;
    case 's':
      if (parse_value(optarg, &scalar)) {
        fprintf(stderr, "invalid scalar\n");
        return 2;
      }
      continue;
    case 'h':
      usage(argv[0]);
      return 0;
    default:
      usage(argv[0]);
      return 2;
    }
    if (parse_size(optarg, target)) {
      fprintf(stderr, "invalid positive integer: %s\n", optarg);
      return 2;
    }
  }
  if (!have_n || optind != argc || n == 0 || n > UINT32_MAX || threads > n ||
      n - 1 > (SIZE_MAX - 1) / stride) {
    usage(argv[0]);
    return 2;
  }
  size_t capacity = (n - 1) * stride + 1;
#if DEFINE == REDUCTION
  if (!path_a) {
    fprintf(stderr, "reduction requires --a\n");
    return 2;
  }
#elif DEFINE == MEMSET
  if (!path_a) {
    fprintf(stderr, "memset requires --a (input file sizes the output)\n");
    return 2;
  }
#elif DEFINE == VECTOR_MULTIPLY
  if (!path_x || !path_y) {
    fprintf(stderr, "vector multiplication requires --x and --y\n");
    return 2;
  }
#elif DEFINE == SCALAR_MULTIPLY
  if (!path_x) {
    fprintf(stderr, "scalar multiplication requires --x\n");
    return 2;
  }
#endif

  kernel_element *a = load_data(path_a, n, "a");
  kernel_element *x = load_data(path_x, n, "x");
  kernel_element *y = load_data(path_y, n, "y");
  if ((path_a && !a) || (path_x && !x) || (path_y && !y)) {
    free(a);
    free(x);
    free(y);
    return 1;
  }
  kernel_element *strided_a = path_a ? calloc(capacity, sizeof(*strided_a)) : NULL;
  kernel_element *strided_x = path_x ? calloc(capacity, sizeof(*strided_x)) : NULL;
  kernel_element *strided_y = path_y ? calloc(capacity, sizeof(*strided_y)) : NULL;
  if ((path_a && !strided_a) || (path_x && !strided_x) ||
      (path_y && !strided_y)) {
    fprintf(stderr, "allocation failed\n");
    free(a);
    free(x);
    free(y);
    free(strided_a);
    free(strided_x);
    free(strided_y);
    return 1;
  }
  if (path_a)
    format_input_data((const char *)a, (char *)strided_a, (uint32_t)n,
                      sizeof(*a), 0, stride);
  if (path_x)
    format_input_data((const char *)x, (char *)strided_x, (uint32_t)n,
                      sizeof(*x), 0, stride);
  if (path_y)
    format_input_data((const char *)y, (char *)strided_y, (uint32_t)n,
                      sizeof(*y), 0, stride);
  free(a);
  free(x);
  free(y);
  a = strided_a;
  x = strided_x;
  y = strided_y;
#if DEFINE == MEMSET
  size_t a_count = 0;
  struct stat st;
  if (stat(path_a, &st) == 0)
    a_count = (size_t)st.st_size / sizeof(kernel_element);
  kernel_element *output = calloc(a_count, sizeof(*output));
#else
  kernel_element *output = calloc(capacity, sizeof(*output));
#endif
  size_t *order = NULL;
  if (random_order) {
    order = malloc(n * sizeof(*order));
    if (order) {
      for (size_t i = 0; i < n; ++i)
        order[i] = i;
      srand((unsigned)seed);
      for (size_t i = n - 1; i > 0; --i) {
        size_t j = (size_t)rand() % (i + 1), tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
      }
    }
  }
  pthread_t *ids = malloc(threads * sizeof(*ids));
  worker_arg *args = calloc(threads, sizeof(*args));
  if (!output || !ids || !args || (random_order && !order)) {
    fprintf(stderr, "allocation failed\n");
    free(a);
    free(x);
    free(y);
    free(output);
    free(order);
    free(ids);
    free(args);
    return 1;
  }
  for (size_t i = 0; i < threads; ++i) {
    args[i].task = (kernel_task){.a = a,
                                 .x = x,
                                 .y = y,
                                 .output = output,
                                 .order = order,
                                 .stride = stride,
                                 .fill_value = fill,
                                 .scalar = scalar};
    args[i].begin = n * i / threads;
    args[i].end = n * (i + 1) / threads;
  }
  perf_Timer timer;
  perf_StartTimer(&timer);
  size_t started = 0;
  for (; started < threads; ++started) {
    int rc = pthread_create(&ids[started], NULL, run_worker, &args[started]);
    if (rc) {
      fprintf(stderr, "pthread_create: %s\n", strerror(rc));
      break;
    }
  }
  for (size_t i = 0; i < started; ++i)
    pthread_join(ids[i], NULL);
  perf_EndTimer(&timer);
  if (started != threads) {
    free(a);
    free(x);
    free(y);
    free(output);
    free(order);
    free(ids);
    free(args);
    return 1;
  }

  printf("kernel=%s elements=%zu stride=%zu pattern=%s threads=%zu "
         "elapsed_ms=%lf",
         kernel_Name(), n, stride, random_order ? "random" : "stride", threads,
         ((double)perf_ComputeNSecDelta(&timer)) / 1e6);
#if DEFINE == REDUCTION
  kernel_element total = 0;
  for (size_t i = 0; i < threads; ++i)
    total += args[i].task.partial_sum;
  printf(" sum=%.9g", (double)total);
#endif
  putchar('\n');
  free(a);
  free(x);
  free(y);
  free(output);
  free(order);
  free(ids);
  free(args);
  return 0;
}
