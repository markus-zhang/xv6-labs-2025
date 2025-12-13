// Mutual exclusion spin locks.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"

#ifdef LAB_LOCK
#define NLOCK 500

static struct spinlock *locks[NLOCK];
struct spinlock lock_locks;
int RWLKDEBUG = 0;
// static int writerawaiting = 0;
// static int rwlkcount = 0;

void
freelock(struct spinlock *lk)
{
  acquire(&lock_locks);
  int i;
  for (i = 0; i < NLOCK; i++) {
    if(locks[i] == lk) {
      locks[i] = 0;
      break;
    }
  }
  release(&lock_locks);
}

static void
findslot(struct spinlock *lk) {
  acquire(&lock_locks);
  int i;
  for (i = 0; i < NLOCK; i++) {
    if(locks[i] == 0) {
      locks[i] = lk;
      release(&lock_locks);
      return;
    }
  }
  panic("findslot");
}
#endif

void
initlock(struct spinlock *lk, char *name)
{
  lk->name = name;
  lk->locked = 0;
  lk->cpu = 0;
#ifdef LAB_LOCK
  lk->nts = 0;
  lk->n = 0;
  findslot(lk);
#endif  
}

// Acquire the lock.
// Loops (spins) until the lock is acquired.
void
acquire(struct spinlock *lk)
{
  push_off(); // disable interrupts to avoid deadlock.
  if(holding(lk))
    panic("acquire");

#ifdef LAB_LOCK
    __sync_fetch_and_add(&(lk->n), 1);
#endif      

  // On RISC-V, sync_lock_test_and_set turns into an atomic swap:
  //   a5 = 1
  //   s1 = &lk->locked
  //   amoswap.w.aq a5, a5, (s1)
  while((__sync_lock_test_and_set(&lk->locked, 1) != 0)) {
#ifdef LAB_LOCK
    __sync_fetch_and_add(&(lk->nts), 1);
#else
   ;
#endif
  }

  // Tell the C compiler and the processor to not move loads or stores
  // past this point, to ensure that the critical section's memory
  // references happen strictly after the lock is acquired.
  // On RISC-V, this emits a fence instruction.
  __sync_synchronize();

  // Record info about lock acquisition for holding() and debugging.
  lk->cpu = mycpu();
}

// Release the lock.
void
release(struct spinlock *lk)
{
  if(!holding(lk))
    panic("release");

  lk->cpu = 0;

  // Tell the C compiler and the CPU to not move loads or stores
  // past this point, to ensure that all the stores in the critical
  // section are visible to other CPUs before the lock is released,
  // and that loads in the critical section occur strictly before
  // the lock is released.
  // On RISC-V, this emits a fence instruction.
  __sync_synchronize();

  // Release the lock, equivalent to lk->locked = 0.
  // This code doesn't use a C assignment, since the C standard
  // implies that an assignment might be implemented with
  // multiple store instructions.
  // On RISC-V, sync_lock_release turns into an atomic swap:
  //   s1 = &lk->locked
  //   amoswap.w zero, zero, (s1)
  __sync_lock_release(&lk->locked);

  pop_off();
}

#ifdef LAB_LOCK
static void
read_acquire_inner(struct rwspinlock *rwlk)
{
  // NOTE: Wait for awaiting writers
  // NOTE: Block writers, not readers once acquired

  int cpu = cpuid();
  // Prevent multiple same CPU read acquire
  // if (__atomic_load_n(&rwlk->rdregistered[cpu], __ATOMIC_RELAXED) != 0)
  //   return;

  // Awaiting for acquired writers to release
  // Awaiting for pending writers to acquire and release
  // Make sure that we do as much as possible in acquire-release
  // But NEVER spin while holding the state lock (acquire without release)
  while (1)
  {
    acquire(&(rwlk->bookkeep));
    if ((rwlk->writerawaiting == 0) && (rwlk->l.locked == 0))
    {
      // OK we can go
      // Increment rdregistered
      rwlk->rdregistered[cpu] += 1;
      release(&(rwlk->bookkeep));
      break;
    }
    release(&(rwlk->bookkeep));
  }
  // while (__atomic_load_n(&(rwlk->writerawaiting), __ATOMIC_SEQ_CST) != 0)
  // {;}

  // while(__atomic_load_n(&(rwlk->l.locked), __ATOMIC_SEQ_CST) != 0)
  // {;}
  // if (RWLKDEBUG)
  //   printf("READER ACQUIRE: writerawaiting %d\n", rwlk->writerawaiting);

  // Block subsequent writers to acquire the lock before it is released
  // Should not block other readers
  // l->locked is inappropriate. It is hard to tell writer-locked from reader-locked.
  
  // Never spin after acquiring the bookkeep lock
  // acquire(&rwlk->bookkeep);
  // __atomic_fetch_add(&(rwlk->readerlocked), 1, __ATOMIC_ACQ_REL);

  // __atomic_fetch_add(&(rwlk->rdregistered[cpu]), 1, __ATOMIC_ACQ_REL);
  // rwlk->rdregistered[cpu] += 1;
  // while((__sync_lock_test_and_set(&rwlk->l.locked, 1) != 0)) {
  //   // If already locked by other readers, don't care, go ahead
  //   if (rwlk->readerlocked)
  //     break;
  // }
  
  // release(&rwlk->bookkeep);
  // Tell the C compiler and the processor to not move loads or stores
  // past this point, to ensure that the critical section's memory
  // references happen strictly after the lock is acquired.
  // On RISC-V, this emits a fence instruction.
  __sync_synchronize();
  if (RWLKDEBUG)
    printf("READER ACQUIRE: CPU %d\n", cpuid());
}

static void
read_release_inner(struct rwspinlock *rwlk)
{
  // NOTE: What if I'm the last reader to release?
  // NOTE: What if I'm not the last reader to release?
  // NOTE: What if a writer tries to acquire the lock at the same time?

  // Since we can have multiple readers/CPUs on the same lock
  // at the same time, we cannot use the original holding()
  // because it checks cpu (which changes with each reader acquire)
  // if(!readerholding(rwlk))
  //   panic("reader release");

  // Return if I did not acquire the lock
  // if (__atomic_load_n(&rwlk->rdregistered[cpu], __ATOMIC_RELAXED) != 1)
  // {
  //   return;
  // }


  // Release the lock, equivalent to lk->locked = 0.
  // This code doesn't use a C assignment, since the C standard
  // implies that an assignment might be implemented with
  // multiple store instructions.
  // On RISC-V, sync_lock_release turns into an atomic swap:
  //   s1 = &lk->locked
  //   amoswap.w zero, zero, (s1)
  // __sync_lock_release(&rwlk->l.locked);

  // rwlk->rdregistered[cpu] = 0;
  acquire(&rwlk->bookkeep);
  int cpu = cpuid();
  // __atomic_fetch_sub(&(rwlk->rdregistered[cpu]), 0, __ATOMIC_RELAXED);
  rwlk->rdregistered[cpu] -= 1;
  // Prevent multiple release without adequate read_acquire()
  if (rwlk->rdregistered[cpu] < 0)
    rwlk->rdregistered[cpu] = 0;
  // Move this towards the end to block writers
  // __atomic_fetch_sub(&(rwlk->readerlocked), 1, __ATOMIC_ACQ_REL);
  release(&rwlk->bookkeep);

  // Tell the C compiler and the CPU to not move loads or stores
  // past this point, to ensure that all the stores in the critical
  // section are visible to other CPUs before the lock is released,
  // and that loads in the critical section occur strictly before
  // the lock is released.
  // On RISC-V, this emits a fence instruction.
  __sync_synchronize();

  if (RWLKDEBUG)
    printf("READER RELEASE: CPU %d\n", cpuid());
}

static void
write_acquire_inner(struct rwspinlock *rwlk)
{
  // NOTE: What if the lock was already acquired by a writer?
  // NOTE: What if the lock was already acquired by a reader?
  // NOTE: What if other writers are waiting?
  // NOTE: What if a read_acquire_inner() is called in middle?
  // Increment writerawaiting in the beginning to prevent reader stealing
  // push_off(); // disable interrupts to avoid deadlock.
  acquire(&rwlk->bookkeep);
  // __atomic_fetch_add(&(rwlk->writerawaiting), 1, __ATOMIC_ACQ_REL);
  rwlk->writerawaiting += 1;
  release(&rwlk->bookkeep);
  // (rwlk->writerawaiting) ++;
  if (RWLKDEBUG)
    printf("WRITER ACQUIRE++: writerawaiting %d\n", rwlk->writerawaiting);

  // Same CPU cannot acquire multiple times without release
  acquire(&rwlk->bookkeep);
  if(holding(&(rwlk->l)))
    panic("write acquire");
  release(&rwlk->bookkeep);

  // while(
  //   // (__atomic_load_n(&(rwlk->readerlocked), __ATOMIC_ACQUIRE) != 0) &&
  //   (__sync_lock_test_and_set(&(rwlk->l.locked), 1) != 0)
  // ) {
  //   ;
  // }
  while (1) 
  {
    // acquire-release in loop to prevent deadlock (do not acquire and spin)
    acquire(&(rwlk->bookkeep));
    if (rwlk->l.locked == 0)
    { 
      int totalreader = 0;
      for (int i = 0; i < NCPU; i++)
      {
        totalreader += rwlk->rdregistered[i];
      }
      if (totalreader == 0)
      {
        // No reader working
        // Now both constraints are satisfied
        rwlk->l.locked = 1;
        struct cpu* c = mycpu();
        rwlk->l.cpu = c;
        // release(&(rwlk->bookkeep));
        break;
      }
    }
    // Some readers are working, continue spionning
    release(&(rwlk->bookkeep));
  }
  release(&(rwlk->bookkeep));
  
  if (RWLKDEBUG)
    printf("WRITER ACQUIRE--: writerawaiting %d\n", rwlk->writerawaiting);
  // __atomic_store_n(&(rwlk->l.cpu), c, __ATOMIC_SEQ_CST);
  // rwlk->l.cpu = c;
  // rwlk->l.locked = 1;
  // int cpu = cpuid();
  // if (RWLKDEBUG)
  //   printf("WRITE ACQUIRE: CPU %d\n", cpu);
  // if (RWLKDEBUG)
  //   printf(
  //     "WRITE ACQUIRE -> locked: %d, CPU: %d, l->cpu: %p, current cpu: %p\n", 
  //     __atomic_load_n(&rwlk->l.locked, __ATOMIC_RELAXED), cpu, rwlk->l.cpu, c
  //   );
  // release(&rwlk->bookkeep);

  // Tell the C compiler and the processor to not move loads or stores
  // past this point, to ensure that the critical section's memory
  // references happen strictly after the lock is acquired.
  // On RISC-V, this emits a fence instruction.
  __sync_synchronize();

  // Record info about lock acquisition for holding() and debugging.
  
}

static void
write_release_inner(struct rwspinlock *rwlk)
{
  acquire(&rwlk->bookkeep);
  if(!holding(&(rwlk->l)))
  {
    printf("WRITE RELEASE PANIC: CPU %d\n", cpuid());
    printf("WRITE RELEASE PANIC -> locked: %d, mycpu(): %p, l->cpu: %p\n", rwlk->l.locked, mycpu(), rwlk->l.cpu);
    panic("write release");
  }
  release(&rwlk->bookkeep);

  // rwlk->l.cpu = 0;
  // acquire(&rwlk->bookkeep);
  // __atomic_store_n(&(rwlk->l.cpu), 0, __ATOMIC_SEQ_CST);
  // rwlk->l.cpu = 0;
  // release(&rwlk->bookkeep);
  // __atomic_fetch_sub(&(rwlk->writerawaiting), 1, __ATOMIC_SEQ_CST);

  // Tell the C compiler and the CPU to not move loads or stores
  // past this point, to ensure that all the stores in the critical
  // section are visible to other CPUs before the lock is released,
  // and that loads in the critical section occur strictly before
  // the lock is released.
  // On RISC-V, this emits a fence instruction.
 

  // Release the lock, equivalent to lk->locked = 0.
  // This code doesn't use a C assignment, since the C standard
  // implies that an assignment might be implemented with
  // multiple store instructions.
  // On RISC-V, sync_lock_release turns into an atomic swap:
  //   s1 = &lk->locked
  //   amoswap.w zero, zero, (s1)
  
  // __sync_lock_release(&rwlk->l.locked);

  // if (RWLKDEBUG)
  //   printf("WRITER RELEASE: writerawaiting %d\n", rwlk->writerawaiting);
  // struct cpu* c = mycpu();
  // int cpu = cpuid();
  
  // if (RWLKDEBUG)
  //   printf("WRITER RELEASE: CPU %d\n", cpu);
  // if (RWLKDEBUG)
  //   printf("WRITE RELEASE -> locked: %d, CPU: %d, l->cpu: %p, current cpu: %p\n", rwlk->l.locked, cpu, rwlk->l.cpu, c);
  
  // Push decrementing writerawaiting towards the end to prevent any reader hijacking
  acquire(&rwlk->bookkeep);
  rwlk->l.cpu = 0;
  rwlk->l.locked = 0;
  // __atomic_fetch_sub(&(rwlk->writerawaiting), 1, __ATOMIC_SEQ_CST);
  rwlk->writerawaiting -= 1;
  release(&rwlk->bookkeep);

   __sync_synchronize();
  // pop_off();
}

void
read_acquire(struct rwspinlock *rwlk)
{
  push_off(); // disable interrupts to avoid deadlock.
  read_acquire_inner(rwlk);
}

void
read_release(struct rwspinlock *rwlk)
{
  read_release_inner(rwlk);
  pop_off();
}

void
write_acquire(struct rwspinlock *rwlk)
{
  push_off(); // disable interrupts to avoid deadlock.
  write_acquire_inner(rwlk);
}

void
write_release(struct rwspinlock *rwlk)
{
  write_release_inner(rwlk);
  pop_off();
}

void
initrwlock(struct rwspinlock *rwlk)
{
  initlock(&rwlk->l, "rwlk");
  initlock(&rwlk->bookkeep, "rwlkbookkeep");

  for (int i = 0; i < NCPU; i++)
  {
    rwlk->rdregistered[i] = 0;
    rwlk->writerawaiting = 0;
    // rwlk->readerlocked = 0;
  }
}

// Test rwspinlock implementation.
static void
rwspinlock_test_step(uint step, const char *msg)
{
  static uint barrier;
  const uint ncpu = 4;

  __atomic_fetch_add(&barrier, 1, __ATOMIC_ACQ_REL);
  while (__atomic_load_n(&barrier, __ATOMIC_RELAXED) < ncpu * step) {
    // spin
  }

  if (cpuid() == 0) {
    printf("rwspinlock_test: step %d: %s\n", step, msg);
  }
}

static uint
delay()
{
  static uint v;
  for (int i = 0; i < 10000; i++) {
    __atomic_fetch_add(&v, 1, __ATOMIC_RELAXED);
  }
  return __atomic_load_n(&v, __ATOMIC_RELAXED);
}

uint64
sys_rwlktest()
{
  int r = 0;
  int step = 0;

  push_off();
  int id = cpuid();

  rwspinlock_test_step(++step, "initrwlock");

  static struct rwspinlock l;
  if (id == 0) {
    initrwlock(&l);
  }

  rwspinlock_test_step(++step, "concurrent read_acquire");

  for (int i = 0; i < 1000000; i++)
    read_acquire(&l);

  rwspinlock_test_step(++step, "concurrent read_release");

  for (int i = 0; i < 1000000; i++)
    read_release(&l);

  rwspinlock_test_step(++step, "prepare read_acquire for writer priority test");

  if (id == 1) {
    for (int i = 0; i < 30; i++) {
      read_acquire(&l);
    }
  }

  rwspinlock_test_step(++step, "writer priority test");

  static uint flag;
  if (id == 0) {
    write_acquire(&l);
    __atomic_store_n(&flag, 1, __ATOMIC_RELAXED);
    write_release(&l);
  }

  if (id == 1) {
    delay();
    for (int i = 0; i < 10; i++) {
      read_release(&l);
    }
    delay();
    for (int i = 0; i < 10; i++) {
      read_release(&l);
    }
    delay();
    for (int i = 0; i < 10; i++) {
      read_release(&l);
    }
  }

  if (id == 2) {
    delay();
    read_acquire(&l);
    uint f = __atomic_load_n(&flag, __ATOMIC_RELAXED);
    if (f == 0) {
      printf("rwspinlock_test: reader sneaked ahead of waiting writer\n");
      r = -1;
    }
    read_release(&l);
  }

  rwspinlock_test_step(++step, "checking for concurrent readers/writers");

  static uint v;
  if (id == 0) {
    uint maxwv = 0;
    for (int i = 0; i < 1000000; i++) {
      write_acquire(&l);
      uint x = __atomic_add_fetch(&v, 1, __ATOMIC_ACQ_REL);
      if (x > maxwv) {
        maxwv = x;
      }
      uint y = __atomic_fetch_sub(&v, 1, __ATOMIC_ACQ_REL);
      if (y > maxwv) {
        maxwv = y;
      }
      write_release(&l);
    }
    if (maxwv > 1) {
      printf("rwspinlock_test: cpu %d saw concurrent reads/writes: %d\n", id, maxwv);
      r = -1;
    }
  } else {
    uint maxrv = 0;
    for (int i = 0; i < 1000000; i++) {
      read_acquire(&l);
      uint x = __atomic_add_fetch(&v, 1, __ATOMIC_ACQ_REL);
      if (x > maxrv) {
        maxrv = x;
      }
      uint y = __atomic_fetch_sub(&v, 1, __ATOMIC_ACQ_REL);
      if (y > maxrv) {
        maxrv = y;
      }
      read_release(&l);
    }
    if (maxrv < 2) {
      printf("rwspinlock_test: cpu %d never saw concurrent reads: %d\n", id, maxrv);
      r = -1;
    }
  }

  rwspinlock_test_step(++step, "checking for concurrent writers");

  uint maxwv = 0;
  for (int i = 0; i < 1000000; i++) {
    write_acquire(&l);
    // printf("i is %d, locked: %d\n", i, l.l.locked);
    uint x = __atomic_add_fetch(&v, 1, __ATOMIC_ACQ_REL);
    if (x > maxwv) {
      maxwv = x;
    }
    uint y = __atomic_fetch_sub(&v, 1, __ATOMIC_ACQ_REL);
    if (y > maxwv) {
      maxwv = y;
    }
    // if (!(l.l.locked))
    //   printf("No idea why it is not locked...\n");
    // printf("i is %d, locked: %d\n", i, l.l.locked);
    write_release(&l);
  }
  if (maxwv > 1) {
    printf("rwspinlock_test: cpu %d saw concurrent writes: %d\n", id, maxwv);
    r = -1;
  }

  rwspinlock_test_step(++step, "acquiring multiple locks");

  // Debug
  RWLKDEBUG = 0;

  struct rwspinlock l2;
  initrwlock(&l2);
  write_acquire(&l2);
  read_acquire(&l);

  rwspinlock_test_step(++step, "releasing multiple locks");

  write_release(&l2);
  read_release(&l);

  // Debug
  RWLKDEBUG = 0;

  for (int i = 0; i < 10; i++) {
    rwspinlock_test_step(++step, "prepare read_acquire for multiple writer priority test");

    static uint writer_count;
    if (id == 3) {
      writer_count = 0;
      read_acquire(&l);
      read_acquire(&l);
    }

    rwspinlock_test_step(++step, "multiple writer priority test");

    if (id == 0 || id == 1) {
      write_acquire(&l);
      writer_count++;
      delay();
      write_release(&l);
    }

    if (id == 2) {
      delay();
      read_acquire(&l);
      if (writer_count == 0) {
        printf("rwspinlock_test: reader sneaked ahead of both waiting writers\n");
        r = -1;
      }
      delay();
      delay();
      delay();
      read_release(&l);
    }

    if (id == 3) {
      delay();
      read_release(&l);
      delay();
      read_release(&l);

      delay();
      delay();

      // By this point, either one writer executed and CPU 2 is holding read lock,
      // or both writers executed.  Should never sneak ahead of second writer.
      read_acquire(&l);
      if (writer_count != 2) {
        printf("rwspinlock_test step %d: reader sneaked ahead of second waiting writer\n", step);
        r = -1;
        // Debug
        // kexit(1);
      }
      read_release(&l);
    }

    // Debug
    RWLKDEBUG = 0;
  }

  rwspinlock_test_step(++step, "done");

  printf("rwspinlock_test(%d): %d\n", id, r);
  // Debug
  RWLKDEBUG = 0;
  pop_off();

  return r;
}
#endif

// Check whether this cpu is holding the lock.
// Interrupts must be off.
int
holding(struct spinlock *lk)
{
  int r;
  r = (lk->locked && lk->cpu == mycpu());
  return r;
}

int
readerholding(struct rwspinlock *rwlk)
{
  int r;
  r = (rwlk->l.locked && rwlk->rdregistered[cpuid()] == 1);
  return r;
}

// push_off/pop_off are like intr_off()/intr_on() except that they are matched:
// it takes two pop_off()s to undo two push_off()s.  Also, if interrupts
// are initially off, then push_off, pop_off leaves them off.

void
push_off(void)
{
  int old = intr_get();

  // disable interrupts to prevent an involuntary context
  // switch while using mycpu().
  intr_off();

  if(mycpu()->noff == 0)
    mycpu()->intena = old;
  mycpu()->noff += 1;
}

void
pop_off(void)
{
  struct cpu *c = mycpu();
  if(intr_get())
    panic("pop_off - interruptible");
  if(c->noff < 1)
    panic("pop_off");
  c->noff -= 1;
  if(c->noff == 0 && c->intena)
    intr_on();
}

// Read a shared 32-bit value without holding a lock
int
atomic_read4(int *addr) {
  uint32 val;
  __atomic_load(addr, &val, __ATOMIC_SEQ_CST);
  return val;
}

#ifdef LAB_LOCK
int
snprint_lock(char *buf, int sz, struct spinlock *lk)
{
  int n = 0;
  if(lk->n > 0) {
    n = snprintf(buf, sz, "lock: %s: #test-and-set %d #acquire() %d\n",
                 lk->name, lk->nts, lk->n);
  }
  return n;
}

int
statslock(char *buf, int sz) {
  int n;
  int tot = 0;

  acquire(&lock_locks);
  n = snprintf(buf, sz, "--- lock kmem stats\n");
  for(int i = 0; i < NLOCK; i++) {
    if(locks[i] == 0)
      break;
    if(strncmp(locks[i]->name, "kmem", strlen("kmem")) == 0) {
      tot += locks[i]->nts;
      n += snprint_lock(buf +n, sz-n, locks[i]);
    }
  }
  
  n += snprintf(buf+n, sz-n, "--- top 5 contended locks:\n");
  int last = 100000000;
  // stupid way to compute top 5 contended locks
  for(int t = 0; t < 5; t++) {
    int top = 0;
    for(int i = 0; i < NLOCK; i++) {
      if(locks[i] == 0)
        break;
      if(locks[i]->nts > locks[top]->nts && locks[i]->nts < last) {
        top = i;
      }
    }
    n += snprint_lock(buf+n, sz-n, locks[top]);
    last = locks[top]->nts;
  }
  n += snprintf(buf+n, sz-n, "tot= %d\n", tot);
  release(&lock_locks);  
  return n;
}
#endif
