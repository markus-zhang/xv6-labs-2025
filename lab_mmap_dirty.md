## Attempt to implement dirty bit

### Trial 1

In xv6-riscv, there are two bits we can use: bit 9 and bit 8. We can use bit-8 for `PTE_D`. We can also combine dirty page for mmap with CoW because each lives in a different VA region.

CoW: bit-8 for PTE_COW, and bit-9 for PTE_WB (writable bit PTE_W backup).

```C
// FEAT -> COW bit 8 to mark COW, and bit 9 to backup PTE_W
#define PTE_COW (1L << 8)
#define PTE_WB (1L << 9)
```

mmap: bit-9 for PTE_D

IIRC, for CoW I implemented a `vmcowfault()` function for write fault handling, which copies the shared pages to the child proc's address space, and reduces the ref count of the original pages. For mmap I implemented a `mmapfault()` function to hijack VAs in the mmap region of the proc's address space. I think both can use the same bits and live together. But I need to find a way to tell the fault handlers which type the page is. Relying on the bits themselves may not be the best idea -- we could say, if bit-9 is 1, AND bit-8 is 0, then it must be a mmap page, and if bit-8 is 1, then it must be a CoW page, and if both are 0, it must be a normal page. **But this won't work if we also implement CoW for mmap.**

Now the question is: how do I set the dirty bit for a page when there is a write operation? If the page has never been mapped/allocated, `mmapfault()` loads it from the file. But the next time the program writes into these pages it does not need to call `mmapfault()` again. I think maybe I can use an array in `fmap`? But how do I do when the region written into is a mmap region except the first time? 

I wrote a `write_test()` function and have stepped into it. I'm sure it does NOT go into `usertrap()` when the pages have already been mapped. If the kernel doesn't get notified when a user proc writes into its own address space, how does it mark the pages as dirty? I have no idea. It is different from CoW because `kfork()` is always called when forking so we can write code in `kfork()`.

```C
void
write_test()
{
  int fd;
  const char * const f = "mmap.dur";

  //create file
  makefile(f);

  if ((fd = open(f, O_RDWR)) == -1)
    err("open (1)");
  
  char *p = mmap(0, PGSIZE*2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);

  printf("write before read\n");
  for (int i = 0; i < PGSIZE*2; i++)
    p[i] = 'Z';

  printf("write a second time, does it invoke mmapfault()?");
  for (int i = 0; i < PGSIZE*2; i++)
    p[i] = 'R';

  munmap(p, PGSIZE*2);

  //Check file content.
  if ((fd = open(f, O_RDONLY)) == -1)
    err("open (1)");
  
  if(read(fd, buf, PGSIZE) != PGSIZE) 
    err("read");

  printf("%s\n", buf);
}
```

One way to go around the limitation, is to force the user programs to NEVER write into any page that has been read before. If the first operation is a read operation, we can mark the page as unwritable. But this doesn't really make any sense, though, so I won't consider this as a solution.

I decide to take a walk and ponder about the question. Eventually I realized that only the MMU knows about which page has been accessed. I then re-read the Page Table chapter of the xv6 book. Bit-7 of a PTE is actually the dirty bit. I'll write a simple syscall to test the idea out. Once the syscall is implemented I can call it from the user land.

```C
//Simple syscall to check the dirty bit -- bit7.
uint64
sys_checkdirty(void)
{
  //Get argument
  vaddr_t addr;
  argaddr(0, &addr);

  struct proc * p = myproc();
  if (!p)
  {
    printf("sys_checkdirty: failed to obtain the proc pointer\n");
    return 0;
  }

  //Get the fmap entry
  int index = findmmapbase(p, addr);
  if (index < 0)
  {
    printf("sys_checkdirty: addr 0x%lx not in mmap region\n", addr);
    return 0;
  }

  pte_t* pte = walk(p->pagetable, addr, 0);

  if (!pte)
  {
    printf("sys_checkdirty: addr 0x%lx not mapped\n", addr);
    return 0;
  }

  return (((*pte) & PTE_D) >> 7);
}
```

And I then added a few lines into `mmap.c`.

```C
  printf("write before read\n");
  printf("dirty bit is: %ld\n", checkdirty((vaddr_t)p));
  for (int i = 0; i < PGSIZE*2; i++)
    p[i] = 'Z';

  printf("dirty bit is: %ld\n", checkdirty((vaddr_t)p));

  printf("write a second time, does it invoke mmapfault()?\n");
  for (int i = 0; i < PGSIZE*2; i++)
    p[i] = 'R';

  printf("dirty bit is: %ld\n", checkdirty((vaddr_t)p));
```

The program prints 0 for the first one, and 1 for the next two. I believe this is the correct implementation. OK now I'm going to use it in `sys_munmap()` -- if the page is dirty, writeback, otherwise we can just return immediately. But I also need a syscall to collect the number of writebacks to measure the number of writebacks saved. I'll also create a user program for mass writebacks -- if we don't use the dirty bit, and only a few writebacks -- if we do use the dirty bit.

### Trial 2

I have written some code for the dirty bit, but then I realized that the original mmap code is not really bullet proof. Remember that there is one TODO about the file offset in `sys_mmap()`? I was not sure whether I should move file offset if `mmap()` is called with a non-zero offset. 