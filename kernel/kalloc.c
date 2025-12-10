// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

// void freerange(void *pa_start, void *pa_end);
void freerange(void *pa_start, void *pa_end, int cpu);
// A debug function
static void kdump(int cpu);
// helper function to find the CPU with min nts
static int minnts(void);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

struct km
{
  struct spinlock lock;
  struct run *freelist;
};

struct km kmems[NCPU];
char* lockname[NCPU] = {"kmem0", "kmem1", "kmem2", "kmem3", "kmem4", "kmem5", "kmem6", "kmem7"};

// kinit() gives 1,000 runs to each CPU from CPU 1 to CPU 7, and dumps the rest to CPU 0
// To achieve this I made three changes to the program:
// 1. Modify kinit() to call a modified freerange
// 2. Modify freerange() to accept a 3rd argument, a pointer to a freelist
// 3. A new function kinitfree(char *, struct run *) that mimics kfree() but for a specific freelist

// void
// kinit()
// {
//   initlock(&kmem.lock, "kmem");
//   freerange(end, (void*)PHYSTOP);
// }

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  char *memstart = end;
  // Assign 1,000 free runs to each CPU (1~7)
  for (int cpu = 1; cpu < NCPU; cpu++)
  {
    // If we want to assign N page, it's from end to end + N * PGSIZE
    // And the next loop starts from address (end + N * PGSIZE)
    // In reality, we only get 999 pages, not 1,000, because memstart is not at page boundary
    initlock(&kmems[cpu].lock, lockname[cpu]);
    freerange(memstart, (void*)(memstart + 1000 * PGSIZE), cpu);
    memstart += 1000 * PGSIZE;
  }
  // Dump the rest to CPU 0
  initlock(&kmems[0].lock, lockname[0]);
  freerange(memstart, (void *)PHYSTOP, 0);
  
  #ifdef DEBUG
  for (int cpu = 0; cpu < NCPU; cpu++)
    kdump(cpu);
  printf("minnts: %d\n", minnts());
  #endif

}

// void
// freerange(void *pa_start, void *pa_end)
// {
//   char *p;
//   p = (char*)PGROUNDUP((uint64)pa_start);
//   for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
//     kfree(p);
// }

void
freerange(void *pa_start, void *pa_end, int cpu)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kinitfree(p, cpu);
}

void
kinitfree(void *pa, int cpu)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP || cpu < 0 || cpu >= NCPU)
    panic("kinitfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmems[cpu].lock);
  r->next = kmems[cpu].freelist;
  kmems[cpu].freelist = r;
  release(&kmems[cpu].lock);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
// void
// kfree(void *pa)
// {
//   struct run *r;

//   if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
//     panic("kfree");

//   // Fill with junk to catch dangling refs.
//   memset(pa, 1, PGSIZE);

//   r = (struct run*)pa;

//   acquire(&kmem.lock);
//   r->next = kmem.freelist;
//   kmem.freelist = r;
//   release(&kmem.lock);
// }

void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();
  int cpu = cpuid();
  pop_off();

  acquire(&kmems[cpu].lock);
  r->next = kmems[cpu].freelist;
  kmems[cpu].freelist = r;
  release(&kmems[cpu].lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// void *
// kalloc(void)
// {
//   struct run *r;

//   acquire(&kmem.lock);
//   r = kmem.freelist;
//   if(r)
//     kmem.freelist = r->next;
//   release(&kmem.lock);

//   if(r)
//     memset((char*)r, 5, PGSIZE); // fill with junk
//   return (void*)r;
// }

void *
kalloc(void)
{
  struct run *r;

  push_off();
  int cpu = cpuid();
  pop_off();

  acquire(&kmems[cpu].lock);
  r = kmems[cpu].freelist;
  if(r)
    kmems[cpu].freelist = r->next;
  release(&kmems[cpu].lock);

  if(!r)
  {
    // Try to steal from preferred CPU
    int prefer = minnts();
    acquire(&kmems[prefer].lock);
    r = kmems[prefer].freelist;
    if (r)
      kmems[prefer].freelist = r->next;
    release(&kmems[prefer].lock);

    // Steal failed, OK now let's steal from other CPUs
    // I want to release() ASAP
    if (!r)
    {
      for (int i = 0; i < NCPU; i++)
      {
        if (i == cpu || i == prefer)
          continue;

        acquire(&kmems[i].lock);
        r = kmems[i].freelist;
        if (r)
        {
          kmems[i].freelist = r->next;
          release(&kmems[i].lock);
          break;
        }
        release(&kmems[i].lock);
      }
    }
  }

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

// Return the index of the CPU that has the minimum nts value for its mem lock
// atomic_read4() should be good enough?
static int
minnts(void)
{
  int nts = atomic_read4(&(kmems[0].lock.nts));
  int cpu = 0;
  for (int i = 1; i < NCPU; i++)
  {
    int ntstemp = atomic_read4(&(kmems[i].lock.nts));
    if (nts > ntstemp)
    {
      nts = ntstemp;
      cpu = i;
    }
  }
  return cpu;
}

// For debugging
static void 
kdump(int cpu)
{
  if (cpu >= NCPU || cpu < 0)
  {
    printf("kdump: cpu out of bound\n");
    return;
  }
  struct run *r = kmems[cpu].freelist;
  int count = 0;
  while (r != 0)
  {
    count++;
    r = r->next;
  }
  printf("CPU%d: %d free runs\n", cpu, count);
}