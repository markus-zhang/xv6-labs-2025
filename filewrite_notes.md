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