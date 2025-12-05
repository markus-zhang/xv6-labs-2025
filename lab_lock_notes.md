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