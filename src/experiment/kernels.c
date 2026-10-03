
#include "inc/kernels.h"

#include <stddef.h>

void kernel_RunRange(kernel_task *task, size_t begin, size_t end) {
#if DEFINE == REDUCTION
  kernel_element sum = 0;
#endif
  for (size_t i = begin; i < end; ++i) {
    size_t position = task->order ? task->order[i] : i;
    size_t index = position * task->stride;
#if defined(DEFINE) && (DEFINE == REDUCTION)
    sum += task->a[index];
#elif defined(DEFINE) && (DEFINE == MEMSET)
    task->output[index] = task->fill_value;
#elif defined(DEFINE) && (DEFINE == VECTOR_MULTIPLY)
    task->output[index] = task->x[index] * task->y[index];
#elif defined(DEFINE) && (DEFINE == SCALAR_MULTIPLY)
    task->output[index] = task->x[index] * task->scalar;
#else
#error "Unsupported DEFINE: use REDUCTION, MEMSET, VECTOR_MULTIPLY, or SCALAR_MULTIPLY"
#endif
  }
#if defined(DEFINE) && (DEFINE == REDUCTION)
  task->partial_sum = sum;
#endif
}

const char *kernel_Name(void) {
#if DEFINE == REDUCTION
  return "array-reduction-sum";
#elif DEFINE == MEMSET
  return "memset";
#elif DEFINE == VECTOR_MULTIPLY
  return "vector-elementwise-multiply";
#elif DEFINE == SCALAR_MULTIPLY
  return "scalar-vector-multiply";
#endif
}
