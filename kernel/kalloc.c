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
void *freeranges(int cpu, void *pa_cpu_start);

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
  int cpu;
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
  void *pa_cpu_start = end;
  for (int i = 0; i < NCPU; i++)
  {
    initlock(&kmems[i].lock, lockname[i]);
    kmems[i].cpu = -1;
    kmems[i].freelist = 0;
    pa_cpu_start = freeranges(i, pa_cpu_start);
  }
}

// void
// freerange(void *pa_start, void *pa_end)
// {
//   char *p;
//   p = (char*)PGROUNDUP((uint64)pa_start);
//   for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
//     kfree(p);
// }

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
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  // Dump the run to CPU0 -- We gotta figure out a way to distribute fairly
  acquire(&kmems[0].lock);
  r->next = kmems[0].freelist;
  kmems[0].freelist = r;
  release(&kmems[0].lock);
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
  int cpu = cpuid();
  struct run *r;

  // Acquire lock as other harts could be getting into kmems[cpu], e.g. stealing its runs
  acquire(&kmems[cpu].lock);
  r = kmems[cpu].freelist;
  if(r)
  {
    // printf("r is 0x%lx, r->next is 0x%lx\n", (uint64)r, (uint64)(r->next));
    kmems[cpu].freelist = r->next;
  }
  // release(&kmems[cpu].lock);

  // if(r)
  //   memset((char*)r, 5, PGSIZE); // fill with junk
  else
  {
    // probably full
    // acquire(&kmems[cpu].lock);
    for (int i = 0; i < NCPU; i++)
    {
      // Skip own CPU
      if (i == cpu)
        continue;
      // I think we need the lock, even if we don't take any run
      acquire(&kmems[i].lock);
      // Naive steal algo: steal from any CPU with available runs
      if (kmems[i].freelist)
      {
        printf("kalloc: CPU %d steals from CPU %d at 0x%lx->0x%lx\n", cpu, i, (uint64)kmems[i].freelist, (uint64)kmems[i].freelist->next);
        r = kmems[i].freelist;
        kmems[cpu].freelist = r;
        // kmems[i].freelist moves first, otherwise next line sets next to 0
        kmems[i].freelist = (kmems[i].freelist)->next;
        printf("kalloc: kmems[i].freelist @ 0x%lx -> 0x%lx\n", (uint64)kmems[i].freelist, (uint64)kmems[i].freelist->next);
        // we are only going to steal one page
        kmems[cpu].freelist->next = 0;
        
        // Once break it won't hit the release() after the if block so we need to do it here too
        release(&kmems[i].lock);
        break;
      }
      // We don't need kmems[i] anymore so can release the lock
      release(&kmems[i].lock);
    }
    // Possible none of the other CPUs has runs so r never gets allocated
    if (r)
      kmems[cpu].allocated += 1;
  }

  // Writing junk data into the page
  if (r)
    memset((char*)r, 5, PGSIZE);
  
  release(&kmems[cpu].lock);
  return (void*)r;
}
