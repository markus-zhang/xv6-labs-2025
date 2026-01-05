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

OK I managed to solve the read fault. But the writeback doesn't work. I traced the problem to `filewrite()`. I have further traced to `either_copyin()` which is called by `writei()`, which is called by `filewrite()`. Looks like there was a failure when `src` is `0x3c00000000`. It was definitely a deadlock.

So after a night of debugging I figured out the source (but I couldn't figure out a solution). Note that before `munmap(p, PGSIZE*2)` the test program runs `_v1(p)`. The `_v1(p)` essentially looks at two pages, and check the contents of the first 1.5 pages against 'A' and the rest of 0.5 pages against 0:

```C
void
_v1(char *p)
{
  int i;
  for (i = 0; i < PGSIZE*2; i++) {
    if (i < PGSIZE + (PGSIZE/2)) {
      if (p[i] != 'A') {
        printf("mismatch at %d, wanted 'A', got 0x%x\n", i, p[i]);
        err("v1 mismatch (1)");
      }
    } else {
      if (p[i] != 0) {
        printf("mismatch at %d, wanted zero, got 0x%x\n", i, p[i]);
        err("v1 mismatch (2)");
      }
    }
  }
}
```

The memory space (`p`) `_v1()` touches was created by `mmap()`:

```C
p = mmap(0, PGSIZE*3, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
```

So what we have is 3 lazily allocated pages and `_v1(p)` checks 2 of them. We know that hitting a lazily allocated page is going to hit `vmfault()`, and because it is a mmap memory region, it in turn goes into `mmapfault()` -- and indeed, if I `printf()` one message for each `mmapfault()` call, I can see that two pages are allocated. One starting from 0x0000003bffffe000 and the other from 0x0000003bfffff000.

```
mmapfault: begin for va 0x0000003bffffe000
mmapfault: read 0x1000 bytes
mmapfault: begin for va 0x0000003bfffff000
mmapfault: read 0x800 bytes
```

Everything works fine till this point. Now, note that the 3rd page has never been physically allocated. But `munmap()` writes back all 3 pages. So once the program tries to write into the 3rd page, `filewrite()` calls `either_copyin()` calls `copyin()`, and in this while loop `pa0` is 0 for the 3rd page because it has never been mapped/allocated. So it tried to go into `vmfault()` to map/allocate the 3rd page. And `vmfault()` calls `fileread()` to load this page. **`filewrite()` locks the inode for each write, and `fileread()` tries to acquire the lock, too, which causes the deadlock.**

```C
  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    //3rd page was never allocated
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }
    //Rest of the code
  }
```

However, once I look at how `munmap()` is getting called in `mmaptest.c`, I realized that only 2 pages are unmapped.

```C
// unmap just the first two of three pages of mapped memory.
if (munmap(p, PGSIZE*2) == -1)
  err("munmap (3)");
```

This led me to think that maybe I should only write back 2 pages. Let me check if it works.

### Trial 3

OK after the change the deadlock was fixed, which makes sense because `filewrite()` never wrote into pages that were not mapped before. However, I got into a new (or old, as I have seen it before) issue:

```
test mmap read/write: OK
test mmap dirty
mmaptest: read returns 0x1000
mmaptest failure: dirty read #2, pid=3
```

I don't get it. How come a `read()` returns half a page?

```C
// check that the writes to the mapped memory were
  // written to the file.
  if ((fd = open(f, O_RDONLY)) == -1)
    err("open (4)");
  if(read(fd, buf, PGSIZE) != PGSIZE)
    err("dirty read #1");
  for (i = 0; i < PGSIZE; i++){
    if (buf[i] != 'B')
      err("file page 0 does not contain modifications");
  }
  int temp = read(fd, buf, PGSIZE);
  // if(read(fd, buf, PGSIZE) != PGSIZE/2)
  if(temp != PGSIZE/2)
  {
    printf("mmaptest: read returns 0x%x\n", temp);
    err("dirty read #2");
  }
```

From what I see in `fileread()`, `f->ip` has size of 0x2000 bytes. This makes sense? Because we just wrote back 2 full pages.

```
(gdb) p *f->ip
$5 = {dev = 0x1, inum = 0x19, ref = 0x3, lock = {locked = 0x1, lk = {locked = 0x0, name = 0x80007730 "sleep lock", cpu = 0x0 <err>}, name
 = 0x800075e8 "inode", pid = 0x3}, valid = 0x1, type = 0x2, major = 0x0, minor = 0x0, nlink = 0x1, size = 0x2000, addrs = {0x3ec, 0x3ed,
0x3ee, 0x3ef, 0x3f0, 0x3f1, 0x3f2, 0x3f3, 0x0, 0x0, 0x0, 0x0, 0x0}}
```

Investigating `bp` (the physical block) in `readi()`, I don't see any problem. In the first for loop, it shows 0x0400 'C' which was written back in `munmap()`.

```
(gdb) p *bp
$10 = {valid = 0x1, disk = 0x0, dev = 0x1, blockno = 0x3f0, lock = {locked = 0x1, lk = {locked = 0x0, name = 0x80007730 "sleep lock", cpu
 = 0x0 <err>}, name = 0x80007558 "buffer", pid = 0x3}, refcnt = 0x1, prev = 0x8001c038 <bcache+25600>, next = 0x80016500 <bcache+2248>, d
ata = 'C' <repeats 1024 times>}
```

In the second loop, it shows 0x0400 'C' as well. Note that `prev` is different from the above one.

```
(gdb) p *bp
$15 = {valid = 0x1, disk = 0x0, dev = 0x1, blockno = 0x3f1, lock = {locked = 0x1, lk = {locked = 0x0, name = 0x80007730 "sleep lock", cpu
 = 0x0 <err>}, name = 0x80007558 "buffer", pid = 0x3}, refcnt = 0x1, prev = 0x80018c18 <bcache+12256>, next = 0x80016500 <bcache+2248>, d
ata = 'C' <repeats 1024 times>}
```

In the third loop, same story.

```
(gdb) p *bp
$18 = {valid = 0x1, disk = 0x0, dev = 0x1, blockno = 0x3f2, lock = {locked = 0x1, lk = {locked = 0x0, name = 0x80007730 "sleep lock", cpu
 = 0x0 <err>}, name = 0x80007558 "buffer", pid = 0x3}, refcnt = 0x1, prev = 0x800194c8 <bcache+14480>, next = 0x80019070 <bcache+13368>,
data = 'C' <repeats 1024 times>}
```

I was out of ideas so I had a discussion with ChatGPT. Apparently `filewrite()` is not supposed to increase the size of the file, which I still don't completely understand the why. But essentially, `filewrite()` should ONLY write back `inode->size` bytes of contents, even if the mmapped array is longer than that. So we only need to make one change of the code:

```C
    //NOTE: `filewrite()` is not supposed to increment file size.
    //So we need to fetch the size first and write properly.
    printf("file size: 0x%x\n", f->ip->size);
    int ret = filewrite(f, baseaddr, f->ip->size);
```

### Trial 4

Now the test goes on to the "test not-mapped unmap" part. In my code it immediately throws a panic:

```C
  //Step 2: Check which mmap region addr belongs to
  struct proc *p = myproc();
  int index = findmmapbase(p, baseaddr);
  if (index == -1)
    panic("sys:munmap: addr not mapped");
```

My pet peeve about these labs is that it doesn't say anything about the specification. I just have to guess what the code should do. The Linux man page says:

> The munmap() system call deletes the mappings for the specified
  address range, and causes further references to addresses within
  the range to generate invalid memory references.  The region is
  also automatically unmapped when the process is terminated.  On
  the other hand, closing the file descriptor does not unmap the
  region.

> The address addr must be a multiple of the page size (but length
  need not be).  All pages containing a part of the indicated range
  are unmapped, and subsequent references to these pages will
  generate SIGSEGV.  It is not an error if the indicated range does
  not contain any mapped pages.

> On success, munmap() returns 0.  On failure, it returns -1, and
  errno is set to indicate the error (probably to EINVAL).

I think this is what `munmap()` is supposed to do -- Check whether the `addr` is in one of the mmap region:
  - If yes, write back, but do not change the size of the file (`inode->size`). Then call `uvmunmap()` to unmap/dealloc the pages.
  - If no, just call `uvmunmap()` to unmap/dealloc the pages.

OK I managed to pass the not-mapped unmap test. But the lazy access has another issues, and it's probably a deadlock, again. So `munmap()` tries to write back and triggers `vmfault()`, which calls `mmapfault()` and calls `fileread()` eventually.

```
test lazy access
sys_mmap: done, mmap region starts from 0x0000003c3fffe000, len 0x2000
mmapfault: begin for va 0x0000003c3fffe000
mmapfault: read 0x1000 bytes
mmapfault: begin for va 0x0000003c3ffff000
```

Here is my thought, maybe `mmapfault()` should load the whole file into memory, once triggered by `if(*p != 'm')`, instead of just one page. Because this will solve the deadlock issue in `munmap()`. `mummap()` should NEVER trigger `vmfault()` during write back, because it locks the inode, and `vmfault()` calls `fileread()` which also tries to acquire the lock in the inode. If all pages were already mapped, `munmap()` then did not need to try to map/allocate. Anyway it is very weird for a write back routine to call `fileread()`...

Hmmm, I'm not sure ^ is a good idea. `mmapfault()` needs to allocate physical pages for mapped VA. So how do you allocate multiple pages in one shot but one of them failed? I mean, it's doable, but it's ugly. 

I discussed with ChatGPT again because I really don't want to add the dirty bit. Eventually I realized that as long as I implement the write back myself, instead of calling `filewrite()`, I'd have control of how many pages I want to write back, without passing an argument to `sys_munmap()`. Just to save some time, I created a special version for `filewrite()`, `writei()` and `copyin()`. Now I'm good for both lazy access test and two files test.

The next objective is to pass fork test. I haven't modified the code for `kfork()` so need to take a look.

### Trial 5

Actually, I found another issue while debugging fork test. It has nothing to do with the fork test specifically, but is a general issue.

The `mmapfault()` code naively calls `fileread()` to load data from file into mmap region when handling a load page fault:

```C
  int bytesread = fileread(f, baseva, PGSIZE);
  if (bytesread <= 0)
    panic("mmapfault: fileread failed!");
```

`fileread()` actually increments `f->off` (offset for `struct file *f`) for each successful call. This works if the load page faults are "in order":
- Assuming file is of 2 pages long. Now read the whole 2 pages, so `mmapfault()` gets triggered, `fileread()` the first page of the file into VA, move offset to the beginning of the second page. `mmapfault()` gets triggered again, `fileread()` the second page of the file into VA+PGSIZE. Everything works as expected.
- Assuming file is of 2 pages long. Now reads the first page, so `mmapfault()` gets triggered, `fileread()` the first page of the file into VA, move offset to the beginning of the second page. Then a second read reads the second page, so `mmapfault()` gets triggered again, etc. Everything works as ^ and is correct.

However, what if the program reads the second page first, and then the first page? In this case, `mmapfault()` gets triggered, but offset is at 0, so `fileread()` still reads the first page, and then move offset to the beginning of the second page. Then `mmapfault()` gets triggered again, and reads the second page. However, this is exactly the opposite order as we wished!

I haven't investigated why this has never caused any issue in the main test, but my theory is, I got away because the on-disk file (created by `makefile()`) is 1.5 pages of 'A', so you can read the 2 pages in any order and still gets the same result -- as long as it doesn't check the whole 2nd page first.

What I'm trying to say is, in `_v1()`, it checks the first 1.5 page, and then a 0.5 page. This is done in order so everything works as expected. However, if it checks the whole 2nd page first -- which means it checks 1) whether the first half page is all 'A', and 2) whether the second half page is all 0, then `mmapfault()` would naively load the first page instead, and we should see an error.

I decided to add a new test and check the ^ theory. I kept the file to be 1.5 pages of 'A'. And I kept the naive logic of `mmapfault()`. `_v2()` tests two indices of the array `p`. The first test checks whether `p[7*PGSIZE/4]` is 0 -- because this is over 1.5 pages (6*PGSIZE/4), it should be 0, not 'A'. The second test checks whether `p[3*PGSIZE/4]` is 'A' -- because this is within 1.5 pages, it should be 'A', not 0. Now, if what I conjectured ^ was correct, both should fail. I also added a bit of code in `fileread()` to print the offset. If what I conjectured ^ was correct, it should print offset 0 for the first load (which is wrong because the test asks for the second page), and offset 0x1000 for the second load (which is again wrong because the test asks for the first page).

```C
void
_v2(char *p)
{
  int i = PGSIZE + 3*PGSIZE/4;
  if (p[i] != 0) 
  {
    printf("mismatch at 0x%x, wanted zero, got 0x%x\n", i, p[i]);
  }
  else
  {
    printf("second 0.5 page test passed\n");
  }
  i = 3*PGSIZE/4;
  if (p[i] != 'A') 
  {
    printf("mismatch at 0x%x, wanted 'A', got 0x%x\n", i, p[i]);
  }
  else
  {
    printf("first 1.5 page test passed\n");
  }
}
```

As expected, both tests failed, and printed what I expected. I can also see that the fault hanlder (`mmapfault()`) naively loaded the first page and then proceeded to the second page.

```
sys_mmap: offset is 0
sys_mmap: done in slot 0, mmap region starts from 0x0000003bffffe000, len 0x2000
mmapfault: begin for va 0x0000003bfffffc00
fileread: f 0x000000008001ff28, offset 0x0
mmapfault: read 0x1000 bytes
mismatch at 0x1C00, wanted zero, got 0x41
mmapfault: begin for va 0x0000003bffffec00
fileread: f 0x000000008001ff28, offset 0x1000
mmapfault: read 0x800 bytes
mismatch at 0xC00, wanted 'A', got 0x0
test reverse mmap read: OK
```

BTW this is also mentioned in one of the hints. Unfortunately the original tests never test the mmap in reverse order so I never bothered to read this hint. I try to rely on myself as much as possible. I shall quote the hint:

> Add code to cause a page-fault in a mmap-ed region to allocate a page of physical memory, read 4096 bytes of the relevant file into that page, and map it into the user address space. Read the file with readi, which takes an offset argument at which to read in the file (but you will have to lock/unlock the inode passed to readi). Don't forget to set the permissions correctly on the page. Run mmaptest; it should get to the first munmap. 

On the other hand, judging from my limited Linux programming experience, it is up to the USER program to control the offset, not the kernel side. Kernel doesn't rewind or change offset UNLESS the user program asks it to do so. Otherwise it keeps increments the file offset for each read.

Now I think there is a way for `mmapfault()` to check the requested offset. For each `p->fmap[]` entry, there is a field called `startua`. This is the VA_START of the beginning of the mmap region. Whenever `mmapfault()` is triggered, we always know the VA of the mmap region. So we can find the different between this VA and VA_START to figure out which page the user program is trying to load.

For example, let's say the mmap region is 10 pages long, starting from 0x1000, till 0xB000. If VA is 0x1000, we know it's the first page, if it's 0x4000, we know it's the 4th page, and so on. And then we can move the offset around accordingly.

### Trial 6

Found another logic error in `sys_munmap()`. TBH sometimes I simply have no idea what the logic should be, and even looking at the test program doesn't give a clear answer. For example, at certain point we need to remove an fma entry, right? Yeah, but when? Should we remove it once the unmapped region SURPASS the file size, or until the whole mmap region has been unmapped? This is a concern when the test program tries to map more regions (e.g. 3 pages) than the file size (1.5 pages).

Judging from the test program. I'm not supposed to remove the fma entry until the whole region has been unmapped, which makes sense. The logic error is that at the end of `sys_munmap()` I did not increment `startua` and reduce `len` of the fma entry properly. I got away because the test program does not test this part. It doesn't test when a fma entry should be removed, and by actually not removing that entry in time, I got away with the 3 page test. Here is the test:

```C
//map 3 pages
p = mmap(0, PGSIZE*3, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
// write the mapped memory.
for (i = 0; i < PGSIZE; i++)
  p[i] = 'B';
for (i = PGSIZE; i < PGSIZE*2; i++)
  p[i] = 'C';

//unmap 2 pages
if (munmap(p, PGSIZE*2) == -1)
  err("munmap (3)");

// unmap the rest of the mapped memory.
if (munmap(p+PGSIZE*2, PGSIZE) == -1)
  err("munmap (4)");
```

In this test, if I remove the entry after the first unmap, just because the file is 1.5 pages long, which is < 2 pages unmapped, the second `munmap()` will result in error because the entry is gone. I actually didn't think much about it, because in my original implementation the code to remove the entry is wrong:

```C
  //Remove the vma entry from p->fmap?
  //We only remove the entry if ALL len has been unmapped
  //When we munmap, make sure offset is cleared too
  //p->fmap[index].f->off = 0;
  if (len >= p->fmap[index].len)
  {
    // printf("Removing fmap entry %d...\n", index);
    p->fmap[index].f->ref -= 1;
    p->fmap[index].f = 0;
    p->fmap[index].flags = 0;
    p->fmap[index].len = 0;
    p->fmap[index].prot = 0;
    p->fmap[index].startua = 0;
    p->totalvma -= 1;
  }
```

This is very wrong, because I did not reduce `p->fmap[index].len` for a partial unmap. Plus I did not increment `p->fmap[index].startua` for each partial unmap. So this entry never got removed (first unmap is 2 pages, second 1 page, neither is greater than 3 pages which is `p->fmap[index].len`, so this `if` branch is never executed). Now I have corrected the code:

```C
//We need to track which part of the mmap region is unmapped.
  //And only when the WHOLE region has been unmapped that we reset the entry.
  //Example: Say we have a mmap region of 3 pages (12KiB).
  //The first call unmaps 1 page (the first page).
  //Should I increment startua by 1 page as well? 
  //The second call unmaps 2 pages. Only by now I remove the entry.
  uint64 newstartua = p->fmap[index].startua + len;
  uint64 endua = p->fmap[index].startua + p->fmap[index].len;
  //If we already unmapped the whole mmap region, remove the entry.
  if (newstartua >= endua)
  {
    p->fmap[index].f->ref -= 1;
    p->fmap[index].f = 0;
    p->fmap[index].flags = 0;
    p->fmap[index].len = 0;
    p->fmap[index].prot = 0;
    p->fmap[index].startua = 0;
    p->totalvma -= 1;
  }
  //Otherwise, simply move startua
  else
  {
    p->fmap[index].startua = newstartua;
    p->fmap[index].len -= len;
  }

```

Everything works fine except for `kexit()` so the fork test is not done yet. I'm going to think about how to modify `kexit()`. During debugging `kexit()`, I found that

- Some `struct file *f` has an unusually high reference count, e.g. 14. 
- Also, `uvmfree()` does not free the mmap regions (or whatever leftover at `kexit()`).

Focusing on the `uvmfree()` issue -- `fork_test()` creates two `fmap` entries, entry 0 and entry 1, both of `len` 2 pages. Then the child unmap 1 page for `p1` (entry 0) and then `exit(0)`. So technically neither of the two entries should be removed by the time it exits. This can be confirmed by the subsequent two calls to `_v1()` -- if `p1` and `p2` are unmapped from their PA then these two calls won't be successful ->

So this leaves us a problem -- during `kfork()`, we copied the whole pagetable from parent to child, so that the child proc can use the same mmap mapping -- this is indeed correct and intended. However, when the child proc exits, `kexit()` marks the child proc as "ZOMBIE", and evetually in the parent process's `kwait()` function, it calls `freeproc()` which calls `proc_freepagetable()` which calls `uvmfree()` ->

`uvmfree()` only calls `uvmunmap()` for the UA starting from 0 and grown by calling `growproc()` (i.e. by incrementing `sz`). It completely ignores the mmap region which has not been unmapped by `sys_munmap()` -- which makes sense too because as we discussed the parent proc still needs them.

Let's recap what the fork test wants to test.

- The child proc calls `_v1(p1)` which should succeed. This means the child proc needs to have the same `fmap` setup, because `vmfault()` looks at the current proc (which is the child proc) and tries to figure out the `fmap` index. If the child proc does not have the same `fmap` entries, then this will fail.

- On the other hand, the child proc can `munmap()` as much as it wants, and this does not impact the parent proc so there is no extra work on my side. The reason is that `munmap()` calls `uvmunmap()` which DOES unmap and free the physical memory. But whence the parent proc calls `_v1()` it's going to trigger `mmapfault()` to allocate the physical addresses again and map them to the user land virtual addresses of `p1` and `p2`, so as long as the `fmap` entries are not disturbed, and if `p1` and `p2` arrays are not disturbed, it should be fine.

- The child proc exits and `uvmfree()` should not panic. Still needs to figure out the exact reason. My conjecture ^ is that `sys_munmap()` called by the child proc never got the chance to unmap/free all 4 pages (VA `p1` and `p2` have 2 pages for each), but only 1 of them, so there are 3 pages mapped to 3 pages of physical addresses. `uvmunmap()` does NOT unmap/free THESE 3 pages because they are beyond the "normal" VA region.


I realized that I probably need to make two changes:

- In `kfork()`, only copy the entries that are not writable (no writeback). For this test I think I can ignore it because it only calls `mmap()` in a readonly fashion (`PROT_READ`). If it's writable, check out `more_test()` to see what the expectation is. 

- In `kexit()`, `uvunmap()` the mmap region. Or modify `uvmfree()` to do the same thing. Gotta think through this to make sure it doesn't break anything.

### Trial 7

OK now I got pass fork test, let's see what `more_test()` is about. This time I'm going to comment the whole test and try to figure out the specification.

```C
void
more_test()
{
  int fd, pid;
  char *p;
  const char * const f = "mmap.dur";
  
  printf("test munmap prevents access\n");
  
  //Make a file of 1.5 pages
  makefile(f);
  if ((fd = open(f, O_RDWR)) == -1)
    err("open");
  //mmap 2 pages, need to writeback when unmapped
  p = mmap(0, PGSIZE*2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED)
    err("mmap");
  close(fd);

  //These two modifications need to be written back when unmapped
  //vmfault() is called for both pages for the parent proc pagetable
  *p = 'X';
  *(p+PGSIZE) = 'Y';

  pid = fork();
  if(pid < 0) err("fork");
  //child proc 1
  if(pid == 0){
    //vmfault() is called for the both pages for child proc 1 pagetable
    //as the PAs are not mapped to child proc 1 PA yet.
    *p = 'a';
    *(p+PGSIZE) = 'b';
    //unmap the second page for child proc 1
    //This determines how should munmap() change mmap region startua and len
    if(munmap(p+PGSIZE, PGSIZE) == -1)
      err("munmap");
    // this should cause a fatal fault
    //NOTE: Why? Because the previous munmap() already removes the mapping
    //and frees the physical memory. This address should not exist in the vma.
    //If the kernel programmer does not implement munmap() correctly(),
    //and this address is still in one of the vma entries,
    //vmfault() will happily call mmapvault() to map and allocate again.
    printf("*(p+PGSIZE) = %x\n", *(p+PGSIZE));
    exit(0);
  }
  int st = 0;
  wait(&st);
  if(st != -1)
    err("child #1 read unmapped memory");

  pid = fork();
  if(pid < 0) err("fork");
  //child proc 2, same story but with the first page unmapped in munmap()
  if(pid == 0){
    *p = 'c';
    *(p+PGSIZE) = 'd';
    if(munmap(p, PGSIZE) == -1)
      err("munmap");
    // this should cause a fatal fault
    printf("*p = %x\n", *p);
    exit(0);
  }
  st = 0;
  wait(&st);
  if(st != -1)
    err("child #2 read unmapped memory");

  // parent should still be able to access the memory.
  //NOTE: for the parent proc, the two pages were mapped and allocated ^.
  //And we didn't touch the vma entries of the parent proc,
  //so these two lines should still work.
  *p = 'P';
  *(p+PGSIZE) = 'Q';

  //munmap() should write back changes for the first page,
  //because mmap() has parameters: PROT_READ | PROT_WRITE, MAP_SHARED
  if(munmap(p, PGSIZE) == -1)
    err("munmap");

  *(p+PGSIZE) = 'R';
  //Same, should write back the second page (just the first half page)
  if(munmap(p+PGSIZE, PGSIZE) == -1)
    err("munmap");

  // read the file, check that the first page starts
  // with P and the second page with R.
  fd = open(f, O_RDONLY);
  if(fd < 0) err("open");
  if(read(fd, buf, PGSIZE) != PGSIZE) err("read");
  if(buf[0] != 'P') err("first byte of file is wrong");
  if(read(fd, buf, PGSIZE) != PGSIZE/2) err("read");
  if(buf[0] != 'R') err("first byte of 2nd page of file is wrong");
  close(fd);

  printf("test munmap prevents access: OK\n");

  printf("test writes to read-only mapped memory\n");

  makefile(f);

  pid = fork();
  if(pid < 0) err("fork");
  if(pid == 0){
    if ((fd = open(f, O_RDWR)) == -1)
      err("open");
    p = mmap(0, PGSIZE*2, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
      err("mmap");
    // this should cause a fatal fault
    //NOTE: This checks whether the child proc copies the PROT/FLAGS correctly
    //This also checks whether we use the PROT/FLAGS to trigger a fatal fault.
    //I need to think about how to achieve that.
    *p = 0;
    exit(*p);
  }

  st = 0;
  wait(&st);
  if(st != -1)
    err("child wrote read-only mapping");

  printf("test writes to read-only mapped memory: OK\n");
}

```

OK eventually I made all the modifications to pass this test. But I got a regression bug: `freewalk()` panics again.

I found out that I never properly `uvmfree()` the child proc in "fork test". Let me explain. I did modify `unvmfree()` to look at the pagetable and then release memory, but here is the twist: `uvmfree()` is called by the parent proc (during `kwait()`) on all of its child procs, so eh, `uvmfree()` is actually freeing the parent proc, not the child proc.

I think a better solution is to move the mmap region free code to `kexit()`. Gotta take a break and take a look tomorrow.