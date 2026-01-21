## TODO List

[X] Write additional tests to test over-mmap. That is, deliberately mmap over EOF and check whether mmap and munmap works properly. It should not impact writeback, and any overmapped region should be filled with `0`, as Linux. ~~Writing back into the overmapped region should either be ignored, or trigger a panic.~~ Writing back into the overmapped region is allowed in Linux. For this test to work, I also need to modify `mmapfault()` or its lower routines to allow read into regions that is not covered by the file, and "read" 0 whenever they are requested by the user program.

[ ] (Sub TODO from above) Other proc should see the changes made to a MAP_SHARED mmap region without munmap() being called. This probably need some significant changes to the code, though, as right now writeback only occurs in munmap(). How can I write back immedaitely?

[X] Convert writeback to a loop per page, without using the dirty bit. The current implementation "seeks" to `BOF + offset`, then copy `nbytes` from user VA `addr` into the file. Basically, break down `nbytes` into chunks of 1 page or less. Note that one of the chunks could be a small chunk, which is under 1 page, if `nbytes` cannot be divided by `PGSIZE`. It needs to pass all previous tests. Write more tests to check whether writing a small chunk spills over to the rest of the page.

[X] Ask ChatGPT to recommend more tests. The more tests we write, the more familiar we are with the code, and we will have fewer bugs.

[X] Modify the code to use dirty bits. Don't forget to write a few syscalls (check the branch `mmap_dirty`) to prove that the dirty bit saves a lot of writebacks. Write more tests...more tests...

## Additional Notes

### Purpose

After reading documents about Linux `mmap` and did a few experiments with it in the user land, I decided to clarify the lab requirement and fix the code based on it. I think this is where true learning occurs -- You look at an existing implementation. You read the manpage, which TBH doesn't really tell you much about the implementation, and neither does it tell you all of the user cases except for one example. You wrote a few user programs to test it out. You then figured out some of its functionalities. And eventually you went back to `xv6` and check how much of the Linux version you can implement within the current kernel.

Ultimately, I want to create a function `mmapwriteback()` that does the following: Given a `struct file *f`, `vaddr_t addr`, `uint64 offset` and `int nbytes`, it writes `nbytes` bytes from user land VA `addr` into the file `f`, starting from offset `offset`.
- `f`     : the `struct file *` pointer that we saved in the `fmap` entry;
- `addr`  : the user land address, e.g. `char *p = mmap(...)`, then `p` is the `addr` passed into this function;
- `offset`: the absolute offset of writeback, i.e. the number of bytes between the start of the file, and the initial address to write back. More details in the examples. Here is an example: Say the mmap maps the 2nd page, the 3rd page and the 4th page against the file `f`. Then we want to write back the last page, then `offset` is 3, not 2, because the start address of the 4th page is 3 pages away from the start of the file.

This implementation supports the following usages. See List 1.1, 1.2, 1.3 and 1.4.

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

**List 1.4: Over-mmap, and write back changes:**

```C
//List 1.4
  //Create mmap region for 20 fpages -- overmmap 10 pages
  p = mmap(0, 30*PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);

  //Write the 4th mpage with '4'.
  for (char *pp = p + 3*PGSIZE; pp < p + 4*PGSIZE; pp++)
  {
    *pp = '4';
  }

  //Write the 10th mpage with '0'.
  for (char *pp = p + 9*PGSIZE; pp < p + 10*PGSIZE; pp++)
  {
    *pp = '0';
  }

  //Write back 5 mpages to file. Should see a bunch of skipped messages.
  if (munmap(p, 5*PGSIZE) == -1)
    err("munmap (5)");

  //Write back 25 mpages to file. Should see a bunch of skipped messages.
  //Also should never spill over to other places.
  if (munmap(p + 5*PGSIZE, 25*PGSIZE) == -1)
    err("munmap (6)");
```

This implementation does NOT support:

User programs **CANNOT** mmap multiple **NON-consecutive** pages in one `mmap()` call. The region has to be consecutive.

User programs need to update the mmap address (in the above case, that is `p`) after each `munmap()` call, if it does not unmap the whole region. For example, in the last Example, note that the first `munmap()` unmaps the 1st page of the 2-page mmap region. The user program must track the change, so the second call starts from `p+PGSIZE`.

User programs **CANNOT** unmap a middle page, to split the mmap region into multiple regions. To achieve this, we would have to use a different type of data structure (e.g. anything that supports splitting nodes), which I do not plan to implement for the near future. Right now, there is no check against this behavior, so the error could be very implicit. I might as well add one soon.

### Write Back

The algorithm of mmap write back can be summarized as simple as List 2.1:

```C
//List 2.1
int
sys_munmap(/* arguments */)
{
  //Step 1: Check whether we need to writeback, and update the variable writeback

  //Step 2: Writeback if writeback is 1.
  if (writeback)
  {
    //Do writeback.
  }
}
```

Step 1 is pretty straightforward. We ONLY write back if `PROT_WRITE`, `MAP_SHARED`, file is opened as `writable`, and the offset from `originalstartua` to `addr` is smaller than the file size. Please read the comments in List 2.2 for more details.

```C
//List 2.2
  //Pre-requisite: prot = PROT_WRITE, flags = MAP_SHARED, and file is writable.
  //Other condition: Do NOTE write back unmapped part (p->fmap[index].startua + p->fmap[index].f->ip->size > addr)
  //For example, the file is of 10.5 pages long. Do we writeback, and how much do we writeback if the 10th page is mmapped?
  if ((
    p->fmap[index].prot & PROT_WRITE) && 
    (p->fmap[index].flags & MAP_SHARED) &&
    p->fmap[index].f->writable &&
    p->fmap[index].originalstartua + p->fmap[index].f->ip->size > addr
  )
    writeback = 1;
```

Step 2 is a bit complicated. We need to calculate the number of bytes to be written back, and then only write back the dirty pages. The whole structure can be summarized as in List 2.3.

```C
//List 2.3
  if(writeback)
  {
    //Step 2.1: Calculate the foffset
    vaddr_t offset = p->fmap[index].offset + (addr - p->fmap[index].startua);
    
    
    //Step 2.2: Calculate total number of bytes to write back (nbytes).

    if (nbytes > 0)
    {
      //Step 2.3: Break down nbytes to multiple PGSIZE nbyteschunk. Loop through every one of them, and only write back the dirty pages.
      for ( ; nbytes > 0; )
      {
        //Step 2.3.1: Write back dirty page. Reset the dirty bit. 

        //Step 2.3.2: Update offset, addr, nbytes and nbyteschunk for the next loop
      }
    }
  }
```

Step 2.1 is not as trivial as it looks like. Assuming there is a 20-page file, and we mmapge 4 fpages: fpage 3 to fpage 6 (we count from fpage 1, not fpage 0). List 2.4 shows the setup.

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

We have mpage 1, 2, 3 and 4 -- again we count from 1, not 0. Let's say we want to write back mpage 2. What is the foffset from the beginning of the file? It is not 2, because the mmap region itself has an offset of 2. So we need to add 2 on top of 2, to get 4. We need to write back mpage 2 into fpage 4. The scheme is to be found in List 2.5. The mmap region offset is saved in `p->fmap[index].offset`, and the mpage offset (regarding the beginning of the mmap region) is to be calculated as (mpage address - the beginning address of the mmap region), which is `(addr - p->fmap[index].startua)`.

```C
//List 2.5
    //Ex. munmap(p+PGSIZE, PGSIZE), and the whole mmap region has an offset,
    //which is saved in p->fmap[index].offset.
    //A second offset, which is the offset of the addr to the start of mmap region,
    //is calculated as offset = p+PGSIZE - p = PGSIZE.
    //Both offsets are then added to be applied to the file position.

    //After the previous munmap, both startua and offset in the fmap entry are updated.
    vaddr_t offset = p->fmap[index].offset + (addr - p->fmap[index].startua);
```

There are other complications that are taken care of, too. Note that the last comment says that both `startua` and `offset` are updated. Why? Because the mmap region may change by one or more incomplete `munmap()` syscall. Let's go back to List 2.4. I'll paste it here for convenience.

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

We now call `munmap()` to unmap the first 2 pages. We arrive at List 2.5.

```C
//List 2.5
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

We immediately notice that the mmap region offset (to the beginning of the file) is changed. Not only that, any mpage left is now 2 pages closer to the beginning address of the mmap region. That's what meant by saying "both startua and offset in the fmap entry are updated".

An additional test of the update of the two offsets is to deliberately over-mmap, and try to write pass the end of the file. List 2.6 demonstrates an example:

```C
//List 2.6
                                                                  ___ End of mmap region
                                                                  |
                                                                  x
|__|__|__|__|__|__|__|__|__|__|__|__|__|__|__|__|__|__|__|__|..|..|
0  1  2  3  4  5  6  7  8  9  10 11 12 13 14 15 16 17 18 19
                                                   x
                                                   |
                                                   |
                                                 Start of mmap region
```

In List 2.6, we create a mmap region of 5 mpages. But only 3 of them are within the file boundary. If we miscalculate the offsets, `munmap()` won't be able to catch a write into the last 2 mpages, which are NOT backed by the file.

Step 2.2 is less tricky, but we still need to make sure that we don't overwrite, shown in List 2.7. A deliberate overwrite PASS the EOF causes `offset` (which is the number of bytes from the beginning address of the file) to be greater than the size of the file. For example, in List 2.6, if we write into mpage 5, the last mpage, `offset` is `22*PGSIZE` bytes. The gotcha part is that `f->ip->size` is a `uint64`, and `offset` is an `int`, so make sure that you don't use subtration which may overflow the result if it is negative. We only need to write back when `offset` is smaller than the size of the file, and we are going to take the minimum of `len` and `f->ip->size - offset`. `len` is one of the arguments, which says the caller wants to unmap `len` bytes. 

Now what does this `min()` do? If `len` is too large, i.e. it causes overwrite, the `min()` truncates it down. Otherwise we just take `len`.

```C
//List 2.7
    //Step 2.2: Calculate total number of bytes to write back (nbytes).
    int nbytes = 0;
    if (offset < f->ip->size)
      nbytes = min(len, f->ip->size - offset);
```

Step 2.3 is shown in full in List 2.8. I have removed some debugging prints to make it a bit more readable. But I keep the commens as they are. We first break down `nbytes` to `nbyteschunk`. Since `nbytes` could be less than `PGSIZE`, we use a second `min()` to make sure the `nbyteschunk` for the first loop is correct. We then go into the loop to check whether the page is dirty. Please note that `addr` is always `PGSIZE` aligned (even after each update, which we will talk about in a minute), so we simply call `walk()` to get the PTE of `addr` and check its `PTE_D` bit. `PTE_D` is the 7th bit and is controlled by the hardware. Once we confirm that the page is dirty, we call `mmapwrite()` to actually write back. We will go into `mmapwrite()` in the next section. Once `mmapwrite()` completes, we need to reset `PTE_D` because I don't think the CPU is smart enough to do that. At the end of the loop we update all variables for the next loop. Note that `addr += nbyteschunk;` is not ALWAYS aligned to `PGSIZE`, but this is fine. Why? Because only the last chunk could be smaller than a page. We simply don't need to use `addr` anymore.

```C
//List 2.8
  //Do not write back if it is in an overmmapped region (> EOF so nbytes keeps 0)
    if (nbytes > 0)
    {
      //Break down nbytes to PGSIZE, so that each mmapwrite() writes a full page if possible.
      //Since addr is already aligned to page, we only need to worry about offset and nbytes.
      //1) vaddr_t offset = p->fmap[index].offset + (addr - p->fmap[index].startua);
      //p->fmap[index].offset is aligned to page as confirmed in sys_mmap().
      //startua is also defined as GiB aligned so definitely page aligned.
      //So we can conclude that offset is aligned to page as well.
      //2) We need to break down nbytes so that each write writes a maximum of PGSIZE bytes.
      //We also use the dirty bit - bit 7 to make sure we can skip the clean ones.
      
      int nbytesbackup = nbytes;
      int nbyteschunk = min(PGSIZE, nbytes);

      for ( ; nbytes > 0; )
      {
        // int nbyteschunk = min(PGSIZE, nbytes);
        //If addr has dirty bit set, then write back, otherwise skipped.
        //addr may not be mapped - if we map 20 pages but only write the first page, 19 pages are not mmaped.
        //Thus we should not panic here, and neither should we allocate the page in walk().

        pte_t *pte = walk(p->pagetable, addr, 0);
        if((pte) && (((*pte) & PTE_D) > 0))
        {
          printf("writeback: writing back 0x%x bytes at 0x%lx offset by 0x%lx.\n", nbyteschunk, addr, offset);
          int ret = mmapwrite(f, addr, offset, nbyteschunk);

          if (ret < 0)
          {
            panic("sys_munmap: file writeback failed");
          }
          
          //Already written the dirty page, so we remove the dirty bit. 
          *pte &= (~PTE_D);
        }

        //We also need to update offset and addr, so that we don't write into/from the same place again and again.
        //So make sure that we don't use offset/addr after this look. That DPRINT() is fine.
        //Still, better to preserve offset/addr and use a copy instead.
        offset += nbyteschunk;
        addr += nbyteschunk;

        //Since we modify nbytes in place, make sure it is not used in the code after this loop.
        nbytes -= nbyteschunk;
        nbyteschunk = min(PGSIZE, nbytes);
      }
    }
```

I want to talk a bit about `mmapwrite()`. It is essemtially a weaker version of `filewrite()` because we only care about `FD_INODE`. Judging from the code, it breaks down into smaller writes. Each write is capped to `n1` because we don't want to exceed the maximum log transaction size. For each `n1`, which is usually of the size of a few `BSIZE`, it is further broken down into buf blocks, either one full block, or partial block if `offset%BSIZE` is not 0. Each "micro" write is handled by `copyin()` which simply copies the data from source, in our case is the mmap region, to the buf block. The process is shown in List 2.9.

```C
//List 2.9
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

I have made a small modification to the code for the situation where `mmap()` is called to map a part of the file with a non-zero offset (mmap from somewhere in the middle of the file, not the beginning of the file). That is, in the lower level `writebacki()` function, I use `offset` instead of `f->off`, because MMAP doesn't really change or use `f->off`.

In List 2.10 I show the lowest level `copyinback()` function. The upper level function call is shown in List 2.10, where `off` is the `offset` argument of `mmapwrite()`. This `offset`, let me remind you, is the total offset of the VA from the beginning address of the file. 

```C
//List 2.10, in writebacki()
if(copyinback(p->pagetable, (void *)(bp->data + (off % BSIZE)), src, m) == -1)
```

```C
//List 2.11
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