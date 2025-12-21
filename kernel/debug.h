#ifndef _DEBUG_H_
#define _DEBUG_H_

#ifndef DEBUG_KERNEL
#define DEBUG_KERNEL 1
#endif

#define ASSERT(cond)                                                        \
  do {                                                                      \
    if (DEBUG_KERNEL) {                                                     \
      if (!(cond)) {                                                        \
        printf("ASSERT failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);   \
        panic("ASSERT");                                                    \
      }                                                                     \
    }                                                                       \
  } while (0)

#endif