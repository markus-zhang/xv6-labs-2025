### More readings in the File System code

**Note 1** - How do I load a file into memory? I use `read()`, which calls `fileread()`. Eventually I get a file descriptor (an integer). A fd is simply the index into a process' `ofile[]` array, each of the element is a `struct file *`.

**Note 2** -  `sys_open()` simply creates 3 fs items:
  - `ip`, which is `struct inode *`.
  - `f`, which is `struct file *`.
  - `fd`, which is simplay an integer index for `proc->ofile[]`, each of the array contains a `struct file *`.

  There is no "loading into memory" in `sys_open()`. I also noticed that `fdalloc()` and `filealloc()` do not check for existance, so a process can `sys_open()` the same file multiple times, which is expected.

### MMAP test program

**Note 1** - `makefile()` makes a file of 1.5 pages of 'A'.

**Note 2** - `mmap_test()`:
  - If a file is opened as RO, `mmap()` it to memory p (a `char *`). Changing the content of the memory should NOT alter the file. There is really no 


**Question 1**: How does the VM write into an opened writable file once the mapped memory region changes? Actually, how does it actually "change" any memory region? What happens during `p[0] = 'Z'`? I already forgot most of the VM system so need to do some debugging. Actually, I probably never figured out the nitty details of the VM.

**Answer 1**: Actually, changing user land VA contents does ot involve the kernel. The CPU simply uses the page table to translate it into PA. 

So I'm thinking, if we are to memory map files into VA 0x0, as the lab mentions, I need to change the page table.

### Ideas about implementation of mmap

1. In the mmap syscall, mappage n pages in the process page table. n = ip->size / PAGESIZE. Then mark all pages as invalid. It should save the relevant informations:
  - start of VA;
  - Assuming contigous memory allocation, we need to know the size of the region;
  - The `fd` so that we can use it to fetch a `struct file *` and use `fileread()` later to read into memory, in the memory read fault handler.
  - Actually, if we keep the `fd` this means we have to keep it open, so maybe just the path, not the `fd` itself.

  - **Q1**: Which virtual addresses should I map to? I have no idea what VAs are left to us. Maybe I can start from the top-ish place? Or maybe start from the top of heap, so everything between TOP of HEAP and TRAPFRAME can be used as mmap region? Gotta read the specifications more closely.

  - **Q2**:

2. In `vmfault()`, always direct it to a special `mmapfault()` first. `mmapfault()` has all information about the mmap so it acts as an agent of `vmfault()` if the VA is in the mmap region. Otherwise it delegates the task back to `vmfault()`. `mmapfault()` knows that it needs to write into user land memory because it is invoked by some user land programs.

OK I'm not sure where to find the VAs for mmap files. Right now the idea is to just increase `p->sz` for say 10 pages and modify `vmfault()` to call some `mmapfault()` first. However this has some issues. First I have no idea if 10 pages is enough or not -- probably not. Next, how do I get the pathname from fd? `namei()` gives fd from pathname, not the reverse. I'm sure there is a way but it's going to be convoluted. 

I don't exactly know how the mmap region should behave once the `fd` is closed. It should still work, though, so I need a way to track pathname, not just the `fd`.

Imagine multiple `mmap()` for the same file. How should `mmap()` behave? 

### Trial 1

OK I managed to pass the basic test. However, I don't know how to get a `sturct file *` after the `fd` is closed, as in the second check.

```C
  // should be able to map file opened read-only with private writable
  // mapping
  p = mmap(0, PGSIZE*2, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (p == MAP_FAILED)
    err("mmap (2)");
  if (close(fd) == -1)
    err("close (1)");
  _v1(p);
  for (i = 0; i < PGSIZE*2; i++)
    p[i] = 'Z';
  if (munmap(p, PGSIZE*2) == -1)
    err("munmap (2)");
  close(fd);
```

Hmmm, I think incrementing `f->ref` in `mmap` and decrementing `f->ref` in `munmap` could fix the problem, but apparently not. I then found out that `sys_close()` closes the file regardless of `f->ref`, so I modified the code a bit.

I got a new problem. I dug in a bit more, and found out that `fileread()` returns 0 bytes read in `mmapfault()`, which is strange. Debugging further, I found that `f->off` retains the previous value. Looks like everytime `fileread()` is called, it moves `f->off` and as long as it is the same `f` it retains the same value. I think this could be the problem.

After a bit of tracing, this is definitely the issue. The function call is:
`readi (ip=0x800167c0 <itable+296>, user_dst=user_dst@entry=1, dst=dst@entry=24576, off=6144, n=n@entry=4096)`

So `off` is 0x1800, and `ip->size` is 0x1800. So eventually `n` is 0, which means 0 bytes read.

```C
  //In readi()
  if(off > ip->size || off + n < off)
    return 0;
  if(off + n > ip->size)
    n = ip->size - off;
```

Here is the proposed solution: every time `close()` is called, even if it does not really close the file due to `f->ref > 0`, it should still reset `f->ref`, because trying to close the file means that the program doesn't want to track `f->ref` anymore and would like to start from the beginning next time. Wow, I actually knew nothing about the offset until now. There is a lot of nitty-gratty details in the FS.

OK now I have a new error: fileclose() panics. I didn't dig deep into this, but looking at the code, looks like every `fileclose()` call reduces `f->ref`, and every `munmap()` reduces `f->ref` too. I think I should create a new variable in `f`, because I'm mixing two reference counts. At least it is cleaner?

I also want to redo the work, because right now I'm simply incrementing `p->sz`. But this leaves a lot of issues. For example, what if say 0x3000 to 0x5000 is mmap region, 0x5000 to 0x6000 is something else, and then we want to munmap the mmap region? **This will leave holes in `p->sz`** and I'm not sure what could be the impact.

TODO list:
[] Map mmap region to a separate VA region
[] Create a new mmap reference for `f`. Actually, the hints says we can reuse `f->ref`, as in `filedup()`, so maybe I should look into whether my code misses a reference count change

### Trial 2

I'm reviewing all references of `p->sz` because I want to take VMA info out of `p->sz` -- i.e. `mmap()` and `munmap()` don't touch `p->sz`. Is it OK? Would it break some existing code? I plunged into a research about `p->sz` by looking at how is it being referenced by the code. Some notes:

- `freeproc()` puts `p->sz` to 0. If we have a separate `struct vma`, `freeproc()` needs to put something in that sutrct to 0, too.

- `growproc()` uses `uvmalloc()` and `uvmdealloc()` to increase/decrease `p->sz`. `p->sz` is not only the total size of proc memory occupied, but also the VA that the proc grows/reduces. xv6 treats user proc addresses as linear -- there is no gap -- `uvmalloc()` always increases `p->sz` and `uvmdealloc()` always decreases `p->sz`.

- `kfork()` also copies `p->sz` to the child process.

- `kexec()` grows memory imprint by using `uvmalloc()` to increment `p->sz`.

- `fetchaddr()` also uses `p->sz` but I don't know exactly how this function is being used, gotta investigate further.

- `sys_sbrk()` increments/decrements `p->sz`. This should be fine because it is a different mechanism to grow/reduce memory.

- `vmfault()` uses `p->sz` to do a check. This is also fine because `mmapfault()` intercepts before this check.

OK I managed to solve the read fault. But the writeback doesn't work. I traced the problem to `filewrite()`, in which `i != n`, so `ret` is -1. 

I have further traced to `either_copyin()` which is called by `writei()`, which is called by `filewrite()`. Looks like there was a failure when `src` is `0x3c00000000`. Not sure what is going on so I decided to dig in further. I think it has something to do with `ip->size`, but I have no idea TBH.

Hmmm, maybe `len` is not updated properly? Because `mmap()` did create the entry with `len` equals `0x3000` (3 pages), but somehow when I inspect the entry in `findmmapwithin()`, `len` was modified to `0x1000`.

I think I found the reason! Somehow, I moved this part before writeback:
```C
  //FIXME: This is WRONG! We need to keep the original len for writeback.
  //Actually I did mention that in the last line, not sure why I forgot about it!
  //Step 3: We need to reduce by len
  //If len reduces to 0, we should mark is as free by setting f to 0,
  //but only after writeback is done
  int oldlen = p->fmap[index].len;  //write back needs to know the total len
  p->fmap[index].len -= len;
```

What's the issue with the ^ code? It reduces `p->fmap[index].len`, so that even when we keep the origin `len` as in `oldlen`, `filewrite()` eventually needs to look at 