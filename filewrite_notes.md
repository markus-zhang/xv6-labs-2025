Here is my reading and analysis of `filewriteback()` and the functions it calls. I need to fully understand the mechanisms behind file writing so that I can rewrite the functionality without using `f->off`. This is going to be used in `sys_munmap()` for writing back.

Ultimately, I want to create a function that does the following: Given a `struct file *f`, `vaddr_t addr`, `uint64 offset` and `int n`, it writes `n` bytes from user land VA `addr` into the file `f`, starting from offset `offset`.

```C
int
mmapwriteback(struct file *f, vaddr_r addr, uint64 offset, int nbytes)
{
  //Step 1: Check whether we spill over the file limit as mmap is not supposed to extend the file, for now.

  //Step 2: In a loop, write a chunk of n1 bytes (see filewriteback()) into the inode, from an incremental offset.
}
```

Step 1 should be pretty straightforward. Assuming file size is `sz`, then check `offset + nbytes <= sz`.

Step 2 is more complicated. Judging from the code, it breaks down into smaller writes. Each write is capped to `n1` because we don't want to exceed the maximum log transaction size. For each `n1`, which is usually of the size of a few `BSIZE`, it is further broken down into buf blocks, either one full block, or partial block if `offset%BSIZE` is not 0. Each "micro" write is handled by `copyin()` which simply copies the data from source, in our case is the mmap region, to the buf block.

```C
int i = 0;
while(i < nbyte)
{
  n1 = maximum bytes to write in one loop;

  begin_op();
  ilock(f->ip);

  //Recall that BSIZE is 1024 bytes while PGSIZE is 4096 bytes
  //Read a buf into memory and write n1 bytes into it
  vaddr_t src = addr + i; //Get VA of source array with offset i, which increments with n1 for each loop
  for (tot=0; tot<n; tot+=m, offset+=m, addr + i)
  {
    bp = a buf block;
    //m could be a full BSIZE, or part of it, if offset%BSIZE > 0, or n-tot, if this is the last for loop
    m = maximum bytes to write into this block; 
    copyin(pagetable, bp->data + offset%BSIZE, src, m);
    //you should never do not increment size -- never exceed file size
  }
  //Now offset is updated too
  iunlock(f->ip);
  end_op();

  i += n1;
}
```

I have made a small modification to the code for the situation where `mmap()` is called to map a part of the file with a non-zero offset.

For example, in `largefile_test()`, a file of 20 pages is created, with each page filled by a letter, starting from 'A' and ending with 'T'. Now a user program calls `mmap()` to mmap s pages starting from page 2.

```C
//mmap 2 pages with offset = 2 pages
p = mmap(0, 2*PGSIZE, PROT_READ, MAP_PRIVATE, fd, 2*PGSIZE);
```
            ___ End of mmap region
            |
            x
|__|__|__|__|__|__|__|__|__|__|__|__|__|__|__|__|__|__|__|__|
0  1  2  3  4  5  6  7  8  9  10 11 12 13 14 15 16 17 18 19
      x
      |
      |
    Start of mmap region

`mmap()` saves the mmap region offset (2 pages in this case) into the `fmap` entry.

When `munmap()` writes back, (if there is a write back and the right permission) it writes back from the start of mmap region towards the EOF -- this is what I called a "full file writeback", although actually it only touches the 3rd page.

```C
//so-called full file writeback
int offset = p->fmap[index].offset + (addr - p->fmap[index].originalstartua);
int nbytes = f->ip->size - offset;
int ret = mmapwrite(f, addr, offset, nbytes);
```

The so called full file writeback writes back every page starting from page 2. Technically, it never writes back unmapped pages, but that is implemented in a lower level routine, not in `sys_munmap()`, so let's pretend it writes back every page starting from page 2.

The calculation of `offset` is a combination of the mmap region offset, and the offset of `addr` to `originalstartua`. Consider the following two `munmap()` calls.

`nbytes` is calculated as full file size - mmap region offset. This is definitely an overkill. In the next iteration I'll change it to `min(len, f->ip->size - offset)`. Not sure why I didn't use `len`, must be stupid. For now, let's keep talking about this imperfect implementation. It still works, just very inefficiently, especially for large files.

```C
//First munmap() call, to unmap the first page.
if (munmap(p, PGSIZE) == -1)
  err("munmap");
```

In this call, `addr` is equal to `originalstartua`, so `offset` is simply the mmap region offset. And `nbytes` is file size (20 pages) - mmap region offset (2 pages) = 18 pages. So the routine writes back 18 pages starting from page 2, which is correct, although inefficient as only 1 page is needed.

After this call, `p->fmap[index].startua` is moved to `p+PGSIZE`, and `p->fmap[index].len` is reduced to 1. Note that `p->fmap[index].len` is the length of the mmap region, which is different from the routine argument `len`. Whenever I talked about `len`, I meant the routine argument. Damn I really need to find better variable names...

```C
//Second munmap() call, to unmap the first page.
if (munmap(p, PGSIZE) == -1)
  err("munmap");
```

In this call, `offset` is calculated as 2 + 1 = 3. We didn't update `p->fmap[index].offset` so it is still 2, but `addr` is one page away from `p->fmap[index].originalstartua`, so the total is 3. I'm thinking, is it better simply to update `p->fmap[index].offset`? So we don't need to add the weird `(addr - p->fmap[index].originalstartua)`.

`nbytes` is calculated as 20 pages - offset (3 pages) = 17 pages.

```C
//FIXME: two things, check TODO in sys_munmap(), 
//1) - update `p->fmap[index].offset` in sys_munmap(), and just use that for the offset at line 822.
//2) - Don't write back all pages from offset to EoF. Write back min(len, f->ip->size - offset) instead.
```