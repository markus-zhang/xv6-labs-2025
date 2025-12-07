// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);
void *freeranges(int cpu, void *pa_cpu_start);
static void kdump(int cpu);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

#ifdef LAB_LOCK
struct km {
  int allocated;
  struct spinlock lock;
  struct run *freelist;
};

struct km kmems[NCPU];
#endif

char* lockname[NCPU] = {"kmem0", "kmem1", "kmem2", "kmem3", "kmem4", "kmem5", "kmem6", "kmem7"};

// void
// kinit()
// {
//   initlock(&kmem.lock, "kmem");
//   freerange(end, (void*)PHYSTOP);
//   #ifdef LAB_LOCK
//   printf("Total free mem: %ld, %lx\n", PHYSTOP - (uint64)end, PHYSTOP - (uint64)end);
//   #endif
// }

void
kinit()
{
  // Initiate kmems, set spu to -1 for initiation
  // void *pa_cpu_start = end;
  for (int i = 0; i < NCPU; i++)
  {
    initlock(&kmems[i].lock, lockname[i]);
    kmems[i].freelist = 0;
    kmems[i].allocated = 0;
    // Give all free runs to CPU 0
    // push_off();
    // int cpu = mycpu();
    // pop_off();
    // pa_cpu_start = freeranges(cpu, pa_cpu_start);
  }
  freerange(end, (void*)PHYSTOP);
  kdump(0);
}

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

// void
// freerange(void *pa_start, void *pa_end)
// {
//   char *p;
//   p = (char*)PGROUNDUP((uint64)pa_start);
//   for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
//     kfree(p);
// }

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  push_off();
  int cpu = cpuid();
  pop_off();
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kcpufree(cpu, p);
    // kfree(p);
}

/*
  NOTE: Total free mem from end to PHYSTOP is 134067856 bytes
  This translates to roughly 32731 pages, and 4092 pages per CPU. The last CPU gets a few less pages but should be fine.
*/
void *
freeranges(int cpu, void *pa_cpu_start)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_cpu_start);
  void *pa_cpu_end = (void *)(p + 4092 * PGSIZE);
  if ((uint64)pa_cpu_end > (uint64)PHYSTOP)
    pa_cpu_end = (void *)PHYSTOP;

  // Last freed page is @ p = pa_cpu_end - PGSIZE
  // This means, for the next CPU, it should start from pa_cpu_end
  // which is exactly the value we return
  for(; p + PGSIZE <= (char *)pa_cpu_end; p += PGSIZE)
    kcpufree(cpu, p);

  return (void *)pa_cpu_end;
}

// kfree() but for each CPU, no need to acquire/release
// because kinit() is only called for CPU0, so no contention
// void
// kcpufree(int cpu, void *pa)
// {
//   struct run *r;

//   if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP || cpu >= NCPU)
//     panic("kcpufree");

//   // Fill with junk to catch dangling refs.
//   memset(pa, 1, PGSIZE);

//   r = (struct run*)pa;

//   // acquire(&kmems[cpu].lock);
//   r->next = kmems[cpu].freelist;
//   kmems[cpu].freelist = r;
//   // release(&kmems[cpu].lock);
// }

void
kcpufree(int cpu, void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP || cpu >= NCPU)
    panic("kcpufree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  // acquire(&kmems[cpu].lock);
  r->next = kmems[cpu].freelist;
  kmems[cpu].freelist = r;
  // release(&kmems[cpu].lock);
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
  push_off();
  int cpu = cpuid();
  pop_off();
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  // Dump the run to myself -- We gotta figure out a way to distribute fairly
  acquire(&kmems[cpu].lock);
  r->next = kmems[cpu].freelist;
  kmems[cpu].freelist = r;
  release(&kmems[cpu].lock);
  // acquire(&kmems[0].lock);
  // r->next = kmems[0].freelist;
  // kmems[0].freelist = r;
  // release(&kmems[0].lock);
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
  push_off();
  int cpu = cpuid();
  pop_off();

  struct run *r;

  // Acquire lock as other harts could be getting into kmems[cpu], e.g. stealing its runs
  acquire(&kmems[cpu].lock);
  r = kmems[cpu].freelist;
  if (r)
    kmems[cpu].freelist = r->next;
  release(&kmems[cpu].lock);

  if (r)
  {
    memset((char*)r, 5, PGSIZE);
    return r;
  }
  else
  {
    // probably no free runs, find another CPU that has some free runs
    for (int i = 0; i < NCPU; i++)
    {
      // Skip own CPU
      if (i == cpu)
        continue;

      // I think we need the lock, even if we don't take any run
      acquire(&kmems[i].lock);

      // Naive steal algo: steal from any CPU with available runs
      if (kmems[i].freelist != 0)
      {
        r = kmems[i].freelist;

        // kmems[cpu].freelist grabs the rest of the list from i
        // Cut off kmems[i].freelist
        // The commented out 2 lines causes hang. Read lablock_notes.md for explanation
        kmems[i].freelist = (kmems[i].freelist)->next;
        // kmems[cpu].freelist = 0;
        // kmems[cpu].freelist = (kmems[i].freelist)->next;
        // kmems[i].freelist = 0;
        
        // Once break it won't hit the release() after the if block so we need to do it here too
        release(&kmems[i].lock);
        break;
      }
      // We don't need kmems[i] anymore so can release the lock
      release(&kmems[i].lock);
    }
    // Possible none of the other CPUs has runs so r never gets allocated
    // if (r)
    //   kmems[cpu].allocated += 1;
  }

  // Writing junk data into the page
  if (r)
    memset((char*)r, 5, PGSIZE);
  
  // release(&kmems[cpu].lock);
  return (void*)r;
}
