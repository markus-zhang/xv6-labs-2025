### Trial 1

I ran `kalloctest`, traced the code and found that it calls `ntas()`, which calls `statistics()` to read a file called `statistics` and print the contents on screen.

I searched the string `statistics` everywhere in the repo and couldn't find it. I got frustrated, and decided to figure it out. I want to know when was this file written into and where it lived. I futilely searched the root for `statistics`, but quickly realized that whatever file created must live within QEMU, in the xv6 file system, not the host Linux file system.

Eventually I tracked down the code in `statslock()` in `spinlock.c` because it prints the same lines shown on screen. Checking its references, I found `statsread()` in `stats.c`. More importantly, if I scroll to the bottom of `stats.c`, I can see the following code:

```C
void
statsinit(void)
{
  initlock(&stats.lock, "stats");

  devsw[STATS].read = statsread;
  devsw[STATS].write = statswrite;
}
```

I think it says that this is part of interrupt code, and `statsread()` is to be called during some device interrupt. So I loaded up `gdb` and put a bp in `statsread()`. I then `c` and ran `kalloctest` in xv6, and the bp was immediately hit. `bt` shows the trace of `usertrap()` -> `syscall()` -> `sys_read` -> `fileread` -> `statsread`. Since it is a read syscall I believe it comes from `statistics()` which calls `read()`.

Then I found that every lock has `n` and `nts`, so I figured they must be incremented somewhere. Since they show the numbers of contentions, I looked into `acquire()` and found the answer:

```C
__sync_fetch_and_add(&(lk->n), 1);
// A few lines below
__sync_fetch_and_add(&(lk->nts), 1);
```

So each `acquire()` increments `n` by 1. Because multiple processes try to acquire the same lock, the author used `__sync_fetch_and_add()` to make sure that there is no racing.

The code flow is: 

- During system initiation, `main()` calls `statsinit()` to install `statsread()` and `statswrite()` as one of the elements of `devsw[]`.

- Every time a process tries to acquire a lock (there are a potential of 500 locks), it increments its `n` and `nts`.

- During the test, user program calls `read()`, which makes a syscall, and trapped into `usertrap()` to call `syscall()`, which calls `fileread()` (which checks type and route to different handlers) to call `statesread()` because the function pointer poining to `statesread()` is put into `devsw[]` by `statsinit()`.

- `statsread()` calls `statslock()` to write into buffer.

Now I still don't know which function created `statistics`, but after the run I found the file within xv6 and contains exactly the texts printed to the screen. There seems to be some issues with the file system as `ls` shows 0 byte and `cat` does print it on screen but shows an error message afterwards.

### Trial 2

Random thoughts:

- ~Figure out how many CPU there is,~ OK we have no idea how many CPUs we have, even though `CPUS` is defined in `Makefile`. `kinit()` is called during initialization of the first hart, and it knows nothing about the other harts.

- I'll initiate `NCPU` elements in a unnamed struct. But I need to consider how best to "steal" blocks from other freelists. So the core problem is, whence `kalloc()` fails to allocate a block for kmem[i], i.e. the freelist for the ith CPU, what is the best way to append blocks from other places.

I know that the total # of free pages between `end` and `PHYSTOP` is about 32,731 pages. Assuming a single free list:

            freelist
                |
Initial status: F->F->...->F->

                  freelist
                     |     
First `kalloc()`: A->F->F...->F->

                                                  freelist
                                                      |  
Eventually all free runs are exhausted: A->A->...->A->

How do we detach free runs from other harts (kmems[j].freelist) and attach it at the end? One way to do it is, let's say `kmems[0].freelist->next == 0` i.e. CPU0 has exhausted the freelist, then we will inquire the next one, i.e. `kmems[1].freelist->next`. We loop through all other CPUs, record their `allocated` number, and stops when we hit the end of the array, or we hit one that has `cpu` is -1 (no CPU), then we append its `freelist` to the end of the previously full freelist. The algo roughly looks like this:

```C
void *
kalloc(void)
{
  int cpu = cpuid();
  struct run *r;

  // Acquire lock as other harts could be getting into kmems[cpu], e.g. stealing its runs
  acquire(&kmems[cpu].lock);
  r = kmems[cpu].freelist;
  if(r)
    kmems[cpu].freelist = r->next;
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
      if (kmems[i].freelist->next)
      {
        printf("kalloc: CPU %d steals from CPU %d\n", cpu, i);
        r = kmems[i].freelist->next;
        kmems[cpu].freelist->next = r;
        kmems[i].freelist->next = 0;
        // release(&kmems[cpu].lock);
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
```

For `kfree()`, I'm also going to use a very naive algo -- it always frees the run to CPU0's freelist. Note that `kfree()` can and will be run by multiple harts, so we need to use lock.

```C
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
```

And we need to initiate each array element. Unfortunately we cannot use `kfree()` because it dumps every free runs into CPU0. Total free mem from end to PHYSTOP is 134067856 bytes. This translates to roughly 32731 pages, and 4092 pages per CPU. The last CPU gets a few less pages but should be fine.

```C
void
kinit()
{
  // Initiate kmems, set spu to -1 for initiation
  void *pa_cpu_start = end;
  for (int i = 0; i < NCPU; i++)
  {
    kmems[i].cpu = -1;
    pa_cpu_start = freeranges(i, pa_cpu_start);
  }
}

void *
freeranges(int cpu, void *pa_cpu_start)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_cpu_start);
  int pa_cpu_end = (uint64)(p + 4092 * PGSIZE);
  if (pa_cpu_end > (uint64)PHYSTOP)
    pa_cpu_end = (uint64)PHYSTOP;

  // Last freed page is @ p = pa_cpu_end - PGSIZE
  // This means, for the next CPU, it should start from pa_cpu_end
  // which is exactly the value we return
  for(; p + PGSIZE <= pa_cpu_end; p += PGSIZE)
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
```