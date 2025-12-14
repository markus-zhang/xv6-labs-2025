#include "param.h"

// Mutual exclusion lock.
struct spinlock {
  uint locked;       // Is the lock held?

  // For debugging:
  char *name;        // Name of lock.
  struct cpu *cpu;   // The cpu holding the lock.
#ifdef LAB_LOCK
  int nts;
  int n;
#endif
};

#ifdef LAB_LOCK
// Reader-writer lock.
// Rules:
// 1. Writers can take the lock from readers
// 2. Only a single writer can acquire the lock
// 3. If there is no writer, all readers can acquire the lock
// Considerations:
// 1. I need to registers the readers in the lock, 
//    so that I can unregister them when writers try to acquire
// 2. Writers should still use l.locked to acquire/release the lock
struct rwspinlock {
  // struct spinlock l;
  char* name;
  struct cpu *cpu;
  struct spinlock bookkeeplk;
  // rfregistered[i] is 1 if CPU i is a reader, 0 if not
  uint rdregistered[NCPU];
  // if a writer tries to acquire, increment
  // once a writer is done, decrement
  // We cannot define a static variable in a C struct
  int writerawaiting;
  int writerlocked;
  // int readerlocked;
};
#endif
