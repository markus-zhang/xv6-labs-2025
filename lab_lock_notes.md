## Lab 1 - Memory Allocator

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

### Trial 3
I absolutely have no idea why the following code causes hanging:

```C
// in kalloc.c when it grabs free runs from other CPU
// The following two lines cause hang
kmems[i].freelist = (kmems[i].freelist)->next;
kmems[cpu].freelist = 0;

// While these two are fine
kmems[cpu].freelist = (kmems[i].freelist)->next;
kmems[i].freelist = 0;
```

The only difference is, `cpu` is the current CPU running the code, so the hanging code keeps its freelist to NULL (0), and moves the `i`th CPU's freelist to its next run. `r` is still valid in both scenarios.

I managed to Ctrl+C in `gdb` during the hang and found out that 3 threads are stuck in `acquire()`, so it has something to do with the locks.

Asked ChatGPT and it identified that it could be deadlocking. It does make sense after I thought about it for a while. So a possible scenario is:

CPU0 is out of free runs, so it tries to acquire the lock of CPU1. At the same time, CPU1 is out of free runs as well, so it tries to acquire the lock of CPU0. The for loop is written in a way that CPU0 looks into CPU1 first and CPU1 looks into CPU0 first. This is a deadlock as each locks itself and seeks to acquire the other lock.

Here are the steps to debug the issue:

First, make sure these two lines are not commented out:

```C
// TODO: I don't know why, but the following 2 lines causes hang:
// It keeps kmems[cpu].freelist as 0 and moves kmems[i].freelist to the next
kmems[i].freelist = (kmems[i].freelist)->next;
kmems[cpu].freelist = 0;
```

In `kalloctest.c`, comment out all tests except `test4()` in `main()`. It is not mandatory but all tests are pretty lengthy so better save some time.

Run `make qemu-gdb` in one window, and then run `gdb-multiarch` in another. Once xv6 brings up the shell, run `kalloctest`. Wait a couple of minutes to make sure that it hangs "properly".

Now, go back to `gdb`, and `CTRL+C` to bring up the command line. Type `info thread`:

```
Id      Target Id                    Frame
* 1    Thread 1.1 (CPU#0 [running]) acquire (lk=lk@entry=0x80008dc8 <kmems+56>) at kernel/spinlock.c:79
  2    Thread 1.2 (CPU#1 [running]) acquire (lk=lk@entry=0x80008d98 <kmems+8>) at kernel/spinlock.c:79
  3    Thread 1.3 (CPU#2 [running]) intr_get () at kernel/riscv.h:304
  4    Thread 1.4 (CPU#3 [running]) acquire (lk=lk@entry=0x80008d98 <kmems+8>) at kernel/spinlock.c:79
```

Thread 1.2 and 1.4 are competing for the same lock. Switch to thread 1.2 by typing `thread 1.2`, and `p *lk` to print:

```
$1 = {locked = 1, name = 0x80008048 "kmem0", cpu = 0x80008f78 <cpus>, nts = -1696434253, n = 54146}
```

This menas it is trying to acquire the lock on CPU0's freelist. Now we switch to `thread 1.1` as it is running in CPU0, and `p *lk` shows that it is trying to acquire the lock on CPU1's freelist:

```
$2 = {locked = 1, name = 0x80008050 "kmem1", cpu = 0x80008ff8 <cpus+128>, nts = 1801419331, n = 62153}
```

So basically we have quite a few contending here:

- Both CPU1 and CPU3 are trying to acquire CPU0's lock. This is probably fine, because as long as one of them acquires the other must spin wait. This alone does not result in the deadlock. Note that at the same time CPU1 and CPU3 hold their own locks.

- However, CPU0 is trying to acquire CPU1's lock. Since CPU1 is holding its own lock, now we have a deadlock. CPU0 can't acquire CPU1's lock, and neither can CPU1 acquire CPU0's lock.

This can also explain why the deadlock did not happen once I switched to the new code:

```C
kmems[cpu].freelist = (kmems[i].freelist)->next;
kmems[i].freelist = 0;
```

So instead of asking for the other CPU every time once it exhausts its own page storage (once per page), it grabs all pages from the other CPU and puts the other CPU out of free pages. This basically means the number of free pages for this CPU is going to last a very long time, so it doesn't have to ask other CPUs for every new page. Using the ^ example, this means less chance of AB/BA deadlock.

OK actually the deadlock still happens, which is annoying. I need to figure out a way to reduce these AB/BA deadlocks.

I did a few modifications:

- ChatGPT reminded me that nested locks can be tricky as they are easy to get into AB/BA deadlocks. So I modified `kalloc()` in two places:

The first change is to release the lock early, to remove AB/BA possibilities.
```C
acquire(&kmems[cpu].lock);
r = kmems[cpu].freelist;
if (r)
  kmems[cpu].freelist = r->next;
// release this lock early
release(&kmems[cpu].lock);

if (r)
{
  memset((char*)r, 5, PGSIZE);
  return r;
}
```

To achieve this, I need to remove any code that deals with `kmems[cpu]`, so I can't append the newly found freelist onto current CPU, which is fine -- I'd just point `r` to the new freelist, update the freelist pointer to its next one, and call it a day.

```C
      // Naive steal algo: steal from any CPU with available runs
      if (kmems[i].freelist != 0)
      {
        r = kmems[i].freelist;

        // Actually, don't do the above, just use this freelist
        // And since we released kmems[cpu].lock ^, can't touch it now
        kmems[i].freelist = (kmems[i].freelist)->next;
        
        // Once break it won't hit the release() after the if block so we need to do it here too
        release(&kmems[i].lock);
        break;
      }
      // We don't need kmems[i] anymore so can release the lock
      release(&kmems[i].lock);

```

- The second modification is to follow the hint to give the CPU that calls `kvinit()` all free runs. There are a bunch of modifications on the top of `kalloc.c` involving this, but nothing really difficult.

**I need to say that although all tests have passed, I'm extremely uncomfortable about my work on this lab:**

- I can never assure myself that the code passes the tests next time;

- Everything seems to be pretty random and I don't know how to quantify things clearly. The tests themselves provide quantification, but much of the process is random (e.g. scheduling). Somehow changing the for loop direction (from CPU 0 to CPU 7, or from CPU 7 to CPU 0) also could result in different results.

Maybe I'll just redo the lab from a clean slate. I need to think through the design.


### Trial 4

OK I'm going to trying it again. This time I'm going to jot down some drafts on paper first.

I found a function called `atomic_read4()` -- Read a shared 32-bit value without holding a lock. This might be useful. So here is the plan:

Change 1: In `freerange()`, it gives each CPU 1,000 free runs to play with. The rest all go to CPU 0.

Change 2: In `kfree()`, it gives the free run to the current CPU. Hopefully this is fair enough, but I need to write some debugging code to measure the number of `kfree()` for each CPU.

Change 3: During each `kalloc()` call, if CPU i has to steal runs from another CPU, it calls `minnts()` to find the least contended CPU mem lock, e.g. CPU j, and it tries to steal from CPU j. If that fails, it loops through all CPUs (except CPU i, but also include CPU j, in case CPU j happens to get a free run).

The code does work, but it failed test4. I tried to postpone `pop_off()` to drag on the interrupts, but I don't think it works either. It does pass the `usertests` tests, though.


### Trial 5

This is going to be my last trial of the first part of this lab. Instead of using `minnts()`, I created another function `minexhausted()` that counts the number of times a CPU exhausts its own free runs. `kalloc()` calls `minexhausted()` to check which CPU to take (with minimum number of exhausted), and tries to steal from it. If it fails to steal then it goes through other CPUs.

I did manage to pass all 4 tests. And I ran it a second time it passes those too. Interesting. Guess this is the trick.


## Lab 2 - Read-Write lock

### Trial 1

I decide to follow the hint and take a look of `sys_rwlktest()`. It has a few steps and look like each step calls `rwspinlock_test_step()` before performing the actual test.

The user land program `rwlktest.c` basically forks a child process for each CPU and runs `sys_rwlktest()`.

I'm trying to understand `rwspinlock_test_step()`:

```C
static void
rwspinlock_test_step(uint step, const char *msg)
{
  static uint barrier;
  const uint ncpu = 4;

  // barrier++ for each CPU, and since it is static all copies of the function use the same variable
  // This is why __stomic_fetch_add() is used
  __atomic_fetch_add(&barrier, 1, __ATOMIC_ACQ_REL);
  // spin until other CPUs have incremented barrier
  while (__atomic_load_n(&barrier, __ATOMIC_RELAXED) < ncpu * step) {
    // spin
  }

  // Only CPU 0 prints the message
  if (cpuid() == 0) {
    printf("rwspinlock_test: step %d: %s\n", step, msg);
  }
}
```

### Trial 2

OK I have found a few problems last night and this morning. I need to clarify the specifications:

1. Any awaiting writer should block subsequent readers. So once a writer calls `acquire()`, it should block subsequent readers.

2. Any writer that already acquired the lock, should block all other readers/writers.

3. Any reader that already acquired the lock, should block subsequent writers. So once a reader confirms that there is no awaiting writers, it should block subsequent writers.

4. Any CPU should be able to run `read_acquire()` mutliple times, **without calling read_release()**, without any issue.

5. Any CPU should be able to run `read_release()` mutliple times, **without calling read_acquire()**, without any issue.

After some debugging, found another wierd issue. Somehow `write_acquire_inner()` does NOT set l->locked to 1. Why? In other cases, mycpu() is different from lk.cpu. How could this be?

### Trial 3

After many trials I managed to get a couple of 4/4 CPU pass for this lab. I'm still not satisfied with the result because it is hit or miss. But here is a summary of what principles I followed:

1. Never wrap while loops with `acquire()-release()`. Otherwise it introduces deadlock (both writers try to contend for the same lock and cannot because the constraints are not satisfied).

2. Always wrap hot pathes with `acquire()-release()`. I introduced a new spinlock `bookkeeplk` for this purpose -- whenever I need to read/update the critical variables, such as `totalreader`, `writerawaiting` and `writerlocked`, I acquire `bookkeeplk` first and release it as soon as possible.

3. For `read_acquire_inner()`, it must spin until:
  - There is no writer working (`writerlocked == 0`)
  - There is no writer awaiting (`writerawaiting == 0`)
  Then it increments `totalreader` to block subsequent writers to acquire the lock

4. For `read_release_inner()`, it simply decrements `totalreader`.

5. For `write_acquire_inner()`, it immediately increments `writerawaiting` to block subsequent readers to steal the lock (recall that readers need to spin until this variable is 0), and the spins until:
  - The last write is done working (`writerlocked == 0`)
  - There is no reader working (`totalreader == 0`)
  Then it sets `writerlocked` to 1, sets `cpu` and decrement `writerawaiting` because it is not waiting anymore.

6. For `write_release_inner()`, it simply sets `cpu` and `writerlocked` to 0. I'm not sure if I should wrap the whole critical part in a single `acquire()-release()`, or (as implemented) in multiples.

The whole implementation is still very shaky. CPU 3 rarely passes the test as a reader can easily sneak ahead of CPU 0 because CPU 0 needs to print a message. But I have no idea how to stop the stealing, since CPU 0 has not even called `write_acquire()`, all those protections are off.