#ifndef KERNELS_H
#define KERNELS_H

#include <stddef.h>

#ifndef DEFINE
#error "Select a kernel with -DDEFINE=REDUCTION, MEMSET, VECTOR_MULTIPLY, or SCALAR_MULTIPLY"
#endif

#define REDUCTION 1
#define MEMSET 2
#define VECTOR_MULTIPLY 3
#define SCALAR_MULTIPLY 4

#if defined(ELEMENT_TYPE_DOUBLE)
typedef double kernel_element;
#else
typedef float kernel_element;
#endif

typedef struct {
  const kernel_element *a;
  const kernel_element *x;
  const kernel_element *y;
  kernel_element *output;
  const size_t *order;       /* NULL for ordered traversal; otherwise a permutation */
  size_t stride;
  kernel_element fill_value;
  kernel_element scalar;
  kernel_element partial_sum;
} kernel_task;

/* Execute logical indices [begin, end) for one worker. */
void kernel_RunRange(kernel_task *task, size_t begin, size_t end);
const char *kernel_Name(void);

#endif /* KERNELS_H */
