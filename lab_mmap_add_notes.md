## TODO List

[X] Write additional tests to test over-mmap. That is, deliberately mmap over EOF and check whether mmap and munmap works properly. It should not impact writeback, and any overmapped region should be filled with `0`, as Linux. ~~Writing back into the overmapped region should either be ignored, or trigger a panic.~~ Writing back into the overmapped region is allowed in Linux. For this test to work, I also need to modify `mmapfault()` or its lower routines to allow read into regions that is not covered by the file, and "read" 0 whenever they are requested by the user program.

[ ] (Sub TODO from above) Other proc should see the changes made to a MAP_SHARED mmap region without munmap() being called. This probably need some significant changes to the code, though, as right now writeback only occurs in munmap(). How can I write back immedaitely?

[ ] Convert writeback to a loop per page, without using the dirty bit. The current implementation "seeks" to `BOF + offset`, then copy `nbytes` from user VA `addr` into the file. Basically, break down `nbytes` into chunks of 1 page or less. Note that one of the chunks could be a small chunk, which is under 1 page, if `nbytes` cannot be divided by `PGSIZE`. It needs to pass all previous tests. Write more tests to check whether writing a small chunk spills over to the rest of the page.

[ ] Ask ChatGPT to recommend more tests. The more tests we write, the more familiar we are with the code, and we will have fewer bugs.

[ ] Modify the code to use dirty bits. Don't forget to write a few syscalls (check the branch `mmap_dirty`) to prove that the dirty bit saves a lot of writebacks. Write more tests...more tests...

## Additional Notes

### Purpose

After reading documents about Linux `mmap` and did a few experiments with it in the user land, I decided to clarify the lab requirement and fix the code based on it. I think this is where true learning occurs -- You look at an existing implementation. You read the manpage, which TBH doesn't really tell you much about the implementation, and neither does it tell you all of the user cases except for one example. You wrote a few user programs to test it out. You then figured out some of its functionalities. And eventually you went back to `xv6` and check how much of the Linux version you can implement within the current kernel.

Ultimately, I want to create a function `mmapwriteback()` that does the following: Given a `struct file *f`, `vaddr_t addr`, `uint64 offset` and `int nbytes`, it writes `nbytes` bytes from user land VA `addr` into the file `f`, starting from offset `offset`.
- `f`     : the `struct file *` pointer that we saved in the `fmap` entry;
- `addr`  : the user land address, e.g. `char *p = mmap(...)`, then `p` is the `addr` passed into this function;
- `offset`: the absolute offset of writeback, i.e. the number of bytes between the start of the file, and the initial address to write back. More details in the examples. Here is an example: Say the mmap maps the 2nd page, the 3rd page and the 4th page against the file `f`. Then we want to write back the last page, then `offset` is 3, not 2, because the start address of the 4th page is 3 pages away from the start of the file.

This implementation supports the following usages. See List 1.1, 1.2 and 1.3.

**List 1.1 - mmap and munmap the nth page of a RDONLY file:**

```C
//List 1.1
  if ((fd = open(f, O_RDONLY)) == -1)
    err("open (1)");

  //Test 1: mmap the 3rd page -- all 'C'.
  printf("largefile test 1: mmap the 3rd page...\n");
  char *p = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 2*PGSIZE);
  if (p == MAP_FAILED)
    err("mmap (1)");
  // printf("*p is: %c\n", *p);
  if (*p != 'C')
    err("mmap (1) the wrong page! Expecting 'C'.");

  if (munmap(p, PGSIZE) == -1)
    err("munmap (1)");
```

**List 1.2 - call `read()` to change the file offset (note that this has nothing to do with the absolute offset we talked above, and neither does it have anything to do with the `mmap()` argument offset), mmap and munmap the nth page of a RDONLY file:**

```C
//List 1.2
  //Test 2: call read() to load the file. This moves f->off.
  //This should not impact mmap()
  printf("largefile test 2: read the 1st page...\n");
  if(read(fd, buf, PGSIZE) != PGSIZE) 
    err("read (1)");

  if (buf[1] != 'A')
    err("read (1) the wrong page! Expecting 'A'");

  printf("largefile test 2: OK\n");

  //Test 3: run test 1 again, f->off changed but should not impact test 1.
  printf("largefile test 3: mmap the 3rd page...\n");
  p = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 2*PGSIZE);
  if (p == MAP_FAILED)
    err("mmap (2)");
  if (*p != 'C')
    err("mmap (2) the wrong page! Expecting 'C'.");

  if (munmap(p, PGSIZE) == -1)
    err("munmap (2)");

  printf("largefile test 3: OK\n");

  close(fd);
```

**List 1.3: mmap multiple consecutive pages of a RDWR file, and write back changes in multiple munmaps:**

```C
//List 1.3
  if ((fd = open(f, O_RDWR)) == -1)
    err("open (1)");

  p = mmap(0, 2*PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 15*PGSIZE);
  if (p == MAP_FAILED)
    err("mmap (5)");

  memset((void *)p, '@', 2*PGSIZE);

  if (munmap(p, PGSIZE) == -1)
    err("munmap (5)");

  //Previous munmap should move startua by PGSIZE
  if (munmap(p+PGSIZE, PGSIZE) == -1)
    err("munmap (6)");

  close(fd);
```

This implementation does NOT support:

User programs **CANNOT** mmap multiple **NON-consecutive** pages in one `mmap()` call. The region has to be consecutive.

User programs need to update the mmap address (in the above case, that is `p`) after each `munmap()` call, if it does not unmap the whole region. For example, in the last Example, note that the first `munmap()` unmaps the 1st page of the 2-page mmap region. The user program must track the change, so the second call starts from `p+PGSIZE`.

User programs **CANNOT** unmap a middle page, to split the mmap region into multiple regions. To achieve this, we would have to use a different type of data structure (e.g. anything that supports splitting nodes), which I do not plan to implement for the near future. Right now, there is no check against this behavior, so the error could be very implicit. I might as well add one soon.

### Write Back reading

The algorithm of mmap write back can be summarized as simple as List 2.1:

```C
//List 2.1
int
mmapwriteback(struct file *f, vaddr_r addr, uint64 offset, int nbytes)
{
  //Step 1: Check whether we spill over the file limit as mmap is not supposed to extend the file, for now.

  //Step 2: In a loop, write a chunk of n1 bytes (see filewriteback()) into the inode, from an incremental offset.
}
```

Step 1 should be pretty straightforward. Assuming file size is `sz`, then check `offset + nbytes <= sz`.

Step 2 is more complicated. Judging from the code, it breaks down into smaller writes. Each write is capped to `n1` because we don't want to exceed the maximum log transaction size. For each `n1`, which is usually of the size of a few `BSIZE`, it is further broken down into buf blocks, either one full block, or partial block if `offset%BSIZE` is not 0. Each "micro" write is handled by `copyin()` which simply copies the data from source, in our case is the mmap region, to the buf block. The process is shown in List 2.2.

```C
//List 2.2
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

For example, in `largefile_test()`, a file of 20 pages is created, with each page filled by a letter, starting from 'A' and ending with 'T'. Now a user program calls `mmap()` to mmap s pages starting from page 2. List 2.3 shows an example of the function call. List 2.4 shows the graphic representation of the file, as well as the mmap region.

```C
//List 2.3
//mmap 2 pages with offset = 2 pages
p = mmap(0, 2*PGSIZE, PROT_READ, MAP_PRIVATE, fd, 2*PGSIZE);
```

```C
//List 2.4
            ___ End of mmap region
            |
            x
|__|__|__|__|__|__|__|__|__|__|__|__|__|__|__|__|__|__|__|__|
0  1  2  3  4  5  6  7  8  9  10 11 12 13 14 15 16 17 18 19
      x
      |
      |
    Start of mmap region
```

`mmap()` saves the mmap region offset (2 pages in this case) into the `fmap` entry.

When `munmap()` writes back, (if there is a write back and the right permission) it writes back from the start of mmap region towards the EOF -- this is what I called a "full file writeback", although actually it only touches the 3rd page.

The so called full file writeback writes back every page starting from page 2. Technically, it never writes back unmapped pages, but that is implemented in a lower level routine, not in `sys_munmap()`, so let's pretend it writes back every page starting from page 2.

### Calculation of offset and nbytes

```C
//List 3.1
//sysfile.c, in sys_munmap(), within the scope of if(writeback).

  //Step 3: Write back in demand.
  if(writeback)
  {
    //...code skipped

    //After the previous munmap, both startua and offset in the fmap entry are updated.
    int offset = p->fmap[index].offset + (addr - p->fmap[index].startua);

    //...code skipped
  }

//sysfile.c, in sys_munmap(), in the else branch of the last if-else.
  else
  {
    //This implementation only supports head/tail unmap.
    //It does not support splitting the mmap region into 2 or more subregions.
    //If the tail is unmmapped, no need to update offset.
    //If the head is unmmapped, move offset to original offset + len
    if (baseaddr == p->fmap[index].startua)
      p->fmap[index].offset += len;

    p->fmap[index].startua = newstartua;
    p->fmap[index].len -= len;
  }
```

The `offset` consists of two parts: Part 1 is what I called the "absolute offset", which is the offset of the whole mmap region to the start of the file. This is represented as `p->fmap[index].offset`. Part 2 is the offset of `addr` to the `startua`. Combine both offsets, we get the real offset that we need to write back into the file. The absolute offset is also updated by each `munmap()`, given that the function call does NOT remove the mmap region entirely -- e.g. removing only 1 page out of a 2-page region. List 3.1 shows the calculation of `offset` and update of `p->fmap[index].offset`.

`nbytes` is calculated as `min(len, f->ip->size - offset)`. Note that there is no dirty bit involved. If the unmapped region (# of bytes given as `len`) is within the file, we simply write back `len` bytes. Now imagine that we deliberately OVER-map, e.g. the file is 20 pages, we map from the 5th page with a length of 30 pages, then when we write back, len = 30 pages, but f->ip->size - offset = 16 pages, so we write back 16 pages eventually. Of course writing back 30 pages won't fail anything because any unmapped region is ignored, but just to be on the safe side.

```C
//List 3.2
//mmap version copyin()
int
copyinback(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    //Ignore unmapped pages
    if(pa0)
      memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}
```

### Example of munmap() with demonstration of writing back

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

In this functio call, `offset` is calculated as 2 + 1 = 3. We didn't update `p->fmap[index].offset` so it is still 2, but `addr` is one page away from `p->fmap[index].originalstartua`, so the total is 3. I'm thinking, is it better simply to update `p->fmap[index].offset`? So we don't need to add the weird `(addr - p->fmap[index].originalstartua)`.

`nbytes` is calculated as 20 pages - offset (3 pages) = 17 pages.