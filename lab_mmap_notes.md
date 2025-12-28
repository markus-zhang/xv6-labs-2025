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