#ifndef _DEBUG_H_
#define _DEBUG_H_

#ifndef DEBUG_KERNEL
#define DEBUG_KERNEL 1
#endif

#ifndef DEBUG_PRINT
#define DEBUG_PRINT 0
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

#if DEBUG_PRINT
  #define DPRINTF(fmt, ...)                                                 \
    do { printf("DEBUG: " fmt, ##__VA_ARGS__); } while (0)
#else
  #define DPRINTF(...) do { } while (0)
#endif

#endif