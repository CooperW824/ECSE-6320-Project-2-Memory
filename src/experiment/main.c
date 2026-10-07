#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "inc/kernels.h"
#include "inc/perf.h"

#include <errno.h>
#include <getopt.h>
#include <linux/perf_event.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif

enum { PERF_L1, PERF_L2, PERF_LLC, PERF_TLB, PERF_METRIC_COUNT };
enum { PERF_READ, PERF_WRITE, PERF_OP_COUNT };
enum { PROFILE_NONE, PROFILE_CACHE, PROFILE_TLB };

static const uint64_t perf_cache_types[PERF_METRIC_COUNT] = {
    PERF_COUNT_HW_CACHE_L1D,
    0, /* Linux's generic perf ABI does not define an L2 cache ID. */
    PERF_COUNT_HW_CACHE_LL,
    PERF_COUNT_HW_CACHE_DTLB};

typedef struct {
  int access_fd;
  int miss_fd;
  int valid;
  int second_event_is_hit_count;
  uint64_t accesses;
  uint64_t misses;
} perf_event_pair;

typedef struct {
  kernel_task task;
  size_t begin, end;
  int profile_mode;
  perf_event_pair counters[PERF_METRIC_COUNT][PERF_OP_COUNT];
} worker_arg;

static int perf_event_open_for_thread(struct perf_event_attr *attr, int group_fd) {
  return (int)syscall(SYS_perf_event_open, attr, 0, -1, group_fd, 0);
}

static int open_hardware_cache_event(uint64_t cache, uint64_t operation,
                                     uint64_t result, int group_fd) {
  struct perf_event_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.type = PERF_TYPE_HW_CACHE;
  attr.size = sizeof(attr);
  attr.config = cache | (operation << 8) | (result << 16);
  attr.disabled = 1;
  /* Reserve the DTLB event pair rather than letting other cache counters
   * multiplex it out of a short kernel invocation. */
  attr.pinned = cache == PERF_COUNT_HW_CACHE_DTLB;
  attr.exclude_kernel = 1;
  attr.exclude_hv = 1;
  attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED |
                     PERF_FORMAT_TOTAL_TIME_RUNNING;
  return perf_event_open_for_thread(&attr, group_fd);
}

static int open_hardware_event(uint64_t event, int group_fd) {
  struct perf_event_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.type = PERF_TYPE_HARDWARE;
  attr.size = sizeof(attr);
  attr.config = event;
  attr.disabled = 1;
  attr.exclude_kernel = 1;
  attr.exclude_hv = 1;
  attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED |
                     PERF_FORMAT_TOTAL_TIME_RUNNING;
  return perf_event_open_for_thread(&attr, group_fd);
}

/* Linux has no generic PERF_COUNT_HW_CACHE_L2 event. This raw AMD Family 1Ah
 * event pair corresponds to perf's l2_cache_req_stat.dc_access_in_l2 and
 * l2_cache_req_stat.dc_hit_in_l2 aliases on the Ryzen AI 7 350 (Family 1Ah,
 * Model 60h). Keep the L2 rate unavailable on other CPUs instead of silently
 * substituting LLC or another cache event. */
static int open_amd_zen5_l2_event(uint64_t config, int group_fd) {
  struct perf_event_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.type = PERF_TYPE_RAW;
  attr.size = sizeof(attr);
  attr.config = config;
  attr.disabled = 1;
  attr.exclude_kernel = 1;
  attr.exclude_hv = 1;
  attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED |
                     PERF_FORMAT_TOTAL_TIME_RUNNING;
  return perf_event_open_for_thread(&attr, group_fd);
}

static int has_amd_zen5_l2_events(void) {
#if defined(__i386__) || defined(__x86_64__)
  unsigned int eax, ebx, ecx, edx;
  if (__get_cpuid(0, &eax, &ebx, &ecx, &edx) == 0)
    return 0;
  char vendor[13];
  memcpy(vendor, &ebx, 4);
  memcpy(vendor + 4, &edx, 4);
  memcpy(vendor + 8, &ecx, 4);
  vendor[12] = '\0';
  if (strcmp(vendor, "AuthenticAMD") != 0 || __get_cpuid(1, &eax, &ebx, &ecx, &edx) == 0)
    return 0;
  unsigned int family = (eax >> 8) & 0xf;
  unsigned int model = (eax >> 4) & 0xf;
  family += (eax >> 20) & 0xff;
  model |= ((eax >> 16) & 0xf) << 4;
  return family == 0x1a && model == 0x60;
#else
  return 0;
#endif
}

static int kernel_uses_operation(size_t operation) {
#if DEFINE == MEMSET
  return operation == PERF_WRITE;
#elif DEFINE == REDUCTION
  return operation == PERF_READ;
#else
  return operation == PERF_READ || operation == PERF_WRITE;
#endif
}

static int metric_uses_operation(size_t metric, size_t operation) {
  /* L2 and generalized LLC counters do not have a read/write selector. */
  if (metric == PERF_L2 || metric == PERF_LLC)
    return operation == PERF_READ;
  /* Report L1 demand-load misses only; L1 store events are not included. */
  if (metric == PERF_L1)
    return operation == PERF_READ && kernel_uses_operation(PERF_READ);
  /* Use one DTLB event pair. Write-side DTLB encodings are not consistently
   * implemented; including them can make the whole rate unavailable. */
  if (metric == PERF_TLB)
    return operation == PERF_READ;
  return kernel_uses_operation(operation);
}

static int profile_includes_metric(int profile_mode, size_t metric) {
  if (profile_mode == PROFILE_CACHE)
    return metric == PERF_L1 || metric == PERF_L2 || metric == PERF_LLC;
  return profile_mode == PROFILE_TLB && metric == PERF_TLB;
}

static void open_worker_counters(worker_arg *arg) {
  for (size_t metric = 0; metric < PERF_METRIC_COUNT; ++metric) {
    for (size_t operation = 0; operation < PERF_OP_COUNT; ++operation) {
      perf_event_pair *pair = &arg->counters[metric][operation];
      pair->access_fd = -1;
      pair->miss_fd = -1;
      if (!profile_includes_metric(arg->profile_mode, metric) ||
          !metric_uses_operation(metric, operation))
        continue;
      if (metric == PERF_L2) {
        if (has_amd_zen5_l2_events()) {
          pair->access_fd = open_amd_zen5_l2_event(UINT64_C(0xf864), -1);
          if (pair->access_fd >= 0)
            pair->miss_fd = open_amd_zen5_l2_event(UINT64_C(0xf064), -1);
          pair->second_event_is_hit_count = 1;
        }
      } else if (metric == PERF_LLC) {
        pair->access_fd = open_hardware_event(PERF_COUNT_HW_CACHE_REFERENCES, -1);
        if (pair->access_fd >= 0)
          pair->miss_fd = open_hardware_event(PERF_COUNT_HW_CACHE_MISSES, -1);
      } else {
        pair->access_fd = open_hardware_cache_event(
            perf_cache_types[metric], operation,
            PERF_COUNT_HW_CACHE_RESULT_ACCESS, -1);
        if (pair->access_fd >= 0)
          pair->miss_fd = open_hardware_cache_event(
              perf_cache_types[metric], operation,
              PERF_COUNT_HW_CACHE_RESULT_MISS, -1);
      }
      if (pair->access_fd < 0 || pair->miss_fd < 0) {
        if (pair->access_fd >= 0)
          close(pair->access_fd);
        if (pair->miss_fd >= 0)
          close(pair->miss_fd);
        pair->access_fd = -1;
        pair->miss_fd = -1;
      } else {
        pair->valid = 1;
      }
    }
  }
}

static void set_worker_counters(worker_arg *arg, unsigned long request) {
  /* DTLB events have limited counter availability on some CPUs. Enable them
   * first so cache events do not starve the requested address-translation
   * measurement when the PMU cannot schedule every event simultaneously. */
  static const size_t priority[PERF_METRIC_COUNT] = {
      PERF_TLB, PERF_L1, PERF_L2, PERF_LLC};
  for (size_t index = 0; index < PERF_METRIC_COUNT; ++index) {
    size_t metric = priority[index];
    for (size_t operation = 0; operation < PERF_OP_COUNT; ++operation) {
      perf_event_pair *pair = &arg->counters[metric][operation];
      if (!profile_includes_metric(arg->profile_mode, metric))
        continue;
      if (pair->access_fd < 0 || pair->miss_fd < 0)
        continue;
      if (request != PERF_EVENT_IOC_DISABLE && !pair->valid)
        continue;
      if (ioctl(pair->access_fd, request, 0) != 0 ||
          ioctl(pair->miss_fd, request, 0) != 0) {
        pair->valid = 0;
        if (request == PERF_EVENT_IOC_ENABLE) {
          ioctl(pair->access_fd, PERF_EVENT_IOC_DISABLE, 0);
          ioctl(pair->miss_fd, PERF_EVENT_IOC_DISABLE, 0);
        }
      }
    }
  }
}

static int read_scaled_event(int fd, uint64_t *value) {
  struct {
    uint64_t value;
    uint64_t time_enabled;
    uint64_t time_running;
  } result;
  if (read(fd, &result, sizeof(result)) != (ssize_t)sizeof(result) ||
      result.time_running == 0)
    return -1;
  long double scaled = (long double)result.value * result.time_enabled /
                       result.time_running;
  *value = scaled > UINT64_MAX ? UINT64_MAX : (uint64_t)scaled;
  return 0;
}

static void collect_worker_counters(worker_arg *arg) {
  for (size_t metric = 0; metric < PERF_METRIC_COUNT; ++metric) {
    for (size_t operation = 0; operation < PERF_OP_COUNT; ++operation) {
      perf_event_pair *pair = &arg->counters[metric][operation];
      if (pair->valid &&
          (read_scaled_event(pair->access_fd, &pair->accesses) != 0 ||
           read_scaled_event(pair->miss_fd, &pair->misses) != 0))
        pair->valid = 0;
      else if (pair->valid && pair->second_event_is_hit_count)
        pair->misses = pair->misses < pair->accesses
                           ? pair->accesses - pair->misses
                           : 0;
    }
  }
}

static void close_worker_counters(worker_arg *arg) {
  for (size_t metric = 0; metric < PERF_METRIC_COUNT; ++metric) {
    for (size_t operation = 0; operation < PERF_OP_COUNT; ++operation) {
      perf_event_pair *pair = &arg->counters[metric][operation];
      if (pair->access_fd >= 0)
        close(pair->access_fd);
      if (pair->miss_fd >= 0)
        close(pair->miss_fd);
      pair->access_fd = pair->miss_fd = -1;
    }
  }
}

static int aggregate_miss_rate(worker_arg *args, size_t threads,
                              int profile_mode, size_t metric, double *rate) {
  if (!profile_includes_metric(profile_mode, metric))
    return 0;
  uint64_t accesses = 0, misses = 0;
  for (size_t operation = 0; operation < PERF_OP_COUNT; ++operation) {
    if (!metric_uses_operation(metric, operation))
      continue;
    for (size_t i = 0; i < threads; ++i) {
      perf_event_pair *pair = &args[i].counters[metric][operation];
      if (!pair->valid)
        return 0;
      accesses += pair->accesses;
      misses += pair->misses;
    }
  }
  if (accesses == 0)
    return 0;
  *rate = 100.0 * (double)misses / (double)accesses;
  return 1;
}

static void print_miss_rate(const char *name, worker_arg *args,
                            size_t threads, int profile_mode, size_t metric) {
  double rate;
  if (aggregate_miss_rate(args, threads, profile_mode, metric, &rate))
    printf(" %s=%.6f%%", name, rate);
  else
    printf(" %s=NA", name);
}

static void usage(const char *program) {
  fprintf(stderr,
          "Usage: %s --n ELEMENTS [--a FILE] [--x FILE] [--y FILE]\n"
          "          [--threads N] [--stride N] [--pattern stride|random]\n"
          "          --profile cache|tlb [--fill VALUE] [--scalar VALUE]\n"
          "          [--seed N]\n"
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
  open_worker_counters(arg);
  set_worker_counters(arg, PERF_EVENT_IOC_RESET);
  set_worker_counters(arg, PERF_EVENT_IOC_ENABLE);
  kernel_RunRange(&arg->task, arg->begin, arg->end);
  set_worker_counters(arg, PERF_EVENT_IOC_DISABLE);
  collect_worker_counters(arg);
  close_worker_counters(arg);
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
  int random_order = 0, have_n = 0, profile_mode = PROFILE_NONE;
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
      {"profile", required_argument, NULL, 'P'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0}};
  int option;
  while ((option = getopt_long(argc, argv, "n:a:x:y:t:d:p:f:s:r:P:h", options,
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
    case 'P':
      if (!strcmp(optarg, "cache"))
        profile_mode = PROFILE_CACHE;
      else if (!strcmp(optarg, "tlb"))
        profile_mode = PROFILE_TLB;
      else {
        fprintf(stderr, "profile must be cache or tlb\n");
        return 2;
      }
      continue;
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
  if (profile_mode == PROFILE_NONE) {
    fprintf(stderr, "--profile cache|tlb is required\n");
    usage(argv[0]);
    return 2;
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
  kernel_element *output = calloc(capacity, sizeof(*output));
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
    args[i].profile_mode = profile_mode;
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
  if (profile_mode == PROFILE_CACHE) {
    print_miss_rate("l1_load_miss_rate", args, threads, profile_mode, PERF_L1);
    print_miss_rate("l2_miss_rate", args, threads, profile_mode, PERF_L2);
    print_miss_rate("llc_miss_rate", args, threads, profile_mode, PERF_LLC);
  } else {
    print_miss_rate("tlb_miss_rate", args, threads, profile_mode, PERF_TLB);
  }
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
