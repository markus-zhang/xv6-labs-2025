## Postmortem of mmap lab

### Introduction

This is the last lab of the series, and touches both memory management and file system, so I figured it worths writing down what I learned, and more importantly, what I *would* do if given a restart.

It is from my experience working on the labs that I learned that kernel programming, and system programming, however elementary they may look like, post significant taxes on the programmer's patience and obssession to correctness.

- Reading the understanding the tests
- Figuring out specification for `mmap` and `munmap`
- Writing the implementation

### Requirement analysis

Since we don't have a written specification for `mmap` and `munmap`, we need to read through and understand the tests to figure that out. I'll jot down as much comment as possible for the three tests.

Such comment focuses on extracting what the tests test for (thus the requirements), what they expect, and what the failing message should be. These comments are prefixed by "NOTE", to differentiate from the "official" comments.

```C
void
mmap_test(void)
{
  int fd;
  int i;
  const char * const f = "mmap.dur";

  //
  // create a file with known content, map it into memory, check that
  // the mapped memory has the same bytes as originally written to the
  // file.
  //
  makefile(f);
  if ((fd = open(f, O_RDONLY)) == -1)
    err("open (1)");

  printf("test basic mmap\n");

  char *p = mmap(0, PGSIZE*2, PROT_READ, MAP_PRIVATE, fd, 0);
  if (p == MAP_FAILED)
    err("mmap (1)");
  _v1(p);
  if (munmap(p, PGSIZE*2) == -1)
    err("munmap (1)");

  printf("test basic mmap: OK\n");

  //NOTE: Test 1 - mmap 2 pages: PROT = RO, FLAGS = MAP_PRIVATE
  //_v1() then reads 2 pages -> expected behavior: reads without issue
  //munmap() then unmaps the 2 pages

  printf("test mmap private\n");

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

  // file should not have been modified.
  if((fd = open(f, O_RDONLY)) < 0) err("open");
  if(read(fd, buf, PGSIZE) != PGSIZE) err("read");
  if(buf[0] != 'A')
    err("write to MAP_PRIVATE was written to file");
  if(read(fd, buf, PGSIZE) != PGSIZE/2) err("read");
  if(buf[0] != 'A')
    err("write to MAP_PRIVATE was written to file");
  close(fd);

  printf("test mmap private: OK\n");

  //NOTE: Test 2 - mmap 2 pages: PROT = RW, FLAGS = MAP_PRIVATE
  //_v1(p) then reads 2 pages -> expected behavior: reads without issue
  //Then writes 'Z' to 2 pages, and munmap(), close(fd)
  //The expected behavior is that the file should NOT be overwritten with 'Z'
  //because FLAGS = MAP_PRIVATE

  printf("test mmap read-only\n");

  // check that mmap doesn't allow read/write mapping of a
  // file opened read-only.
  if ((fd = open(f, O_RDONLY)) == -1)
    err("open (2)");
  p = mmap(0, PGSIZE*2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p != MAP_FAILED)
    err("mmap (3)");
  if (close(fd) == -1)
    err("close (2)");

  printf("test mmap read-only: OK\n");

  //NOTE: Test 3 - mmap 2 pages: PROT = RW, FLAGS = MAP_SHARED
  //But the file is opened as RO. 
  //So the expected behavior is that mmap() should return -1

  printf("test mmap read/write\n");

  // check that mmap does allow read/write mapping of a
  // file opened read/write.
  if ((fd = open(f, O_RDWR)) == -1)
    err("open (3)");
  p = mmap(0, PGSIZE*3, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED)
    err("mmap (4)");
  if (close(fd) == -1)
    err("close (3)");

  // check that the mapping still works after close(fd).
  _v1(p);

  // write the mapped memory.
  for (i = 0; i < PGSIZE; i++)
    p[i] = 'B';
  for (i = PGSIZE; i < PGSIZE*2; i++)
    p[i] = 'C';

  // unmap just the first two of three pages of mapped memory.
  if (munmap(p, PGSIZE*2) == -1)
    err("munmap (3)");

  printf("test mmap read/write: OK\n");

  printf("test mmap dirty\n");

  // check that the writes to the mapped memory were
  // written to the file.
  if ((fd = open(f, O_RDONLY)) == -1)
    err("open (4)");
  int temp = read(fd, buf, PGSIZE);
  if(temp != PGSIZE)
    err("dirty read #1");
  for (i = 0; i < PGSIZE; i++){
    if (buf[i] != 'B')
      err("file page 0 does not contain modifications");
  }
  temp = read(fd, buf, PGSIZE);
  if(temp != PGSIZE/2)
  {
    printf("mmaptest: read returns 0x%x\n", temp);
    err("dirty read #2");
  }
  for (i = 0; i < PGSIZE/2; i++){
    if (buf[i] != 'C')
      err("file page 1 does not contain modifications");
  }
  if (close(fd) == -1)
    err("close (4)");

  printf("test mmap dirty: OK\n");

  //NOTE: Test 4 - mmap 3 pages: PROT = RW, FLAGS = MAP_SHARED
  //Then close(fd). Then _v1(p) reads the first 2 pages.
  //Then write 'B' to the first page, and 'C' into the second page.
  //Then munmap() the first 2 pages.
  //Then read() and checks whether the first page is all 'B'.
  //Note that read() retains the offset, so the second read() actually starts from the beginning of the 2nd page,
  //which read just 0.5 pages.
  //Then continue read() and checks whether the first half of the second page is all 'C'
  //Then close(fd)
  //This is one of the more difficult tests, and we can summarize the expectations as:
  //1) If PROT=RW, FLAGS=MAP_SHARED, we should write back any changes made to the mmap region.
  //2) However, we should never "extend" the file in write back.
  //You can see that the program checks whether the first read() reads 1 page and the second reads 0.5 pages.
  //If we extend the file, then the second read() is going to read a full 1 page.
  //(The lab hints note that we can always write back the full file without worrying about the dirty bit)

  printf("test not-mapped unmap\n");

  // unmap the rest of the mapped memory.
  if (munmap(p+PGSIZE*2, PGSIZE) == -1)
    err("munmap (4)");

  printf("test not-mapped unmap: OK\n");

  //NOTE: Test 5 - munmap() the 3rd page (recall that we mmap 3 pages ^?)
  //Not much to say here. The expected result is that we should be able to
  //munmap() any region mapped by mmap()

  printf("test lazy access\n");

  if(unlink(f) != 0) err("unlink");
  makefile(f);

  if ((fd = open(f, O_RDWR)) == -1)
    err("open");
  p = mmap(0, PGSIZE*2, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED)
    err("mmap");
  close(fd);

  // mmap() should not have read the file at this point,
  // so that the file modification we're about to make
  // ought to be visible to a subsequent read of the
  // mapped memory.

  if((fd = open(f, O_RDWR)) == -1)
    err("open");
  if(write(fd, "m", 1) != 1)
    err("write");
  close(fd);

  //Immediately traps into vmfault() because p is lazily allocated
  if(*p != 'm')
    err("read was not lazy");

  if(munmap(p, PGSIZE*2) == -1)
    err("munmap");

  printf("test lazy access: OK\n");

  //NOTE: Test 6 - The program makes the file anew.
  //Then it mmap() 2 pages: PROT = RW, FLAGS = MAP_SHARED
  //Then close(fd).
  //Then write() 'm' (just 1 byte) into the file.
  //Then reads from *p and checks whether it is 'm'.
  //Then munmap() the 2 pages.
  //The expectation is: mmap() should never pre-load the file into the mapped pages.
  //Instead, the kernel relies on fault handlers to load the file once needed.
  //The code that triggers the handler is if(*p != 'm').

  printf("test mmap two files\n");

  //
  // mmap two different files at the same time.
  //
  int fd1;
  if((fd1 = open("mmap1", O_RDWR|O_CREATE)) < 0)
    err("open (5)");
  if(write(fd1, "12345", 5) != 5)
    err("write (1)");
  char *p1 = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd1, 0);
  if(p1 == MAP_FAILED)
    err("mmap (5)");
  if (close(fd1) == -1)
    err("close (5)");
  if (unlink("mmap1") == -1)
    err("unlink (1)");

  int fd2;
  if((fd2 = open("mmap2", O_RDWR|O_CREATE)) < 0)
    err("open (6)");
  if(write(fd2, "67890", 5) != 5)
    err("write (2)");
  char *p2 = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd2, 0);
  if(p2 == MAP_FAILED)
    err("mmap (6)");
  if (close(fd2) == -1)
    err("close (6)");
  if (unlink("mmap2") == -1)
    err("unlink (2)");

  if(memcmp(p1, "12345", 5) != 0)
    err("mmap1 mismatch");
  if(memcmp(p2, "67890", 5) != 0)
    err("mmap2 mismatch");

  if (munmap(p1, PGSIZE) == -1)
    err("munmap (5)");
  if(memcmp(p2, "67890", 5) != 0)
    err("mmap2 mismatch (2)");
  if (munmap(p2, PGSIZE) == -1)
    err("munmap (6)");

  printf("test mmap two files: OK\n");

  //NOTE: Test 7 - mmap 1 page for two files
  //This tests whether the kernel knows how to handle multiple VMAs.
  //If the programmer only implements one VMA, then this test fails.
}
```

```C
void
fork_test(void)
{
  int fd;
  int pid;
  const char * const f = "mmap.dur";

  printf("test fork\n");

  // mmap the file twice.
  makefile(f);
  if ((fd = open(f, O_RDONLY)) == -1)
    err("open (7)");
  if (unlink(f) == -1)
    err("unlink (3)");
  char *p1 = mmap(0, PGSIZE*2, PROT_READ, MAP_SHARED, fd, 0);
  if (p1 == MAP_FAILED)
    err("mmap (7)");
  char *p2 = mmap(0, PGSIZE*2, PROT_READ, MAP_SHARED, fd, 0);
  if (p2 == MAP_FAILED)
    err("mmap (8)");

  // read just 2nd page.
  if(*(p1+PGSIZE) != 'A')
    err("fork mismatch (1)");

  if((pid = fork()) < 0)
    err("fork");
  if (pid == 0) {
    //_v1() triggers mmapfault(), which looks at p->fmap.
    //So child proc needs to have the same fmap as its parent's.
    _v1(p1);
    if (munmap(p1, PGSIZE) == -1) // just the first page
      err("munmap (7)");
    printf("pid: %d\n", getpid());
    exit(0); // tell the parent that the mapping looks OK.
  }

  int status = -1;
  wait(&status);

  if(status != 0){
    printf("fork_test failed\n");
    exit(1);
  }

  // check that the parent's mappings are still there.
  _v1(p1);
  _v1(p2);

  printf("test fork: OK\n");

  //NOTE: This is a small test. The parent proc mmap twice first, and then fork.
  //In the child proc, _v1() triggers vmfault() and it is supposed to work as expected.
  //This gives out the answer -- the child proc needs to copy the VMA data from the parent proc.
  //One interesting thing is that the child proc just munmap() one page from p1 and then exit().
  //Recall that the parent proc during kawiat() would release the memory of any zombie child proc,
  //using uvmfree(), so we need to make sure that uvmfree() takes care of mmap regions
  //that has not been released by munmap(). If we don't do so, freewalk() is going to panic,
  //because it relies on uvunmap() to release the leaf before releasing the page tables themselves.
  //A smaller requirement is that exiting the child proc should not impact the parent proc.
  //Well since we are copying data, this should be fine.
}
```

```C
void
more_test()
{
  int fd, pid;
  char *p;
  const char * const f = "mmap.dur";
  
  printf("test munmap prevents access\n");
  
  makefile(f);
  if ((fd = open(f, O_RDWR)) == -1)
    err("open");
  p = mmap(0, PGSIZE*2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED)
    err("mmap");
  close(fd);

  *p = 'X';
  *(p+PGSIZE) = 'Y';

  pid = fork();
  if(pid < 0) err("fork");
  if(pid == 0){
    *p = 'a';
    *(p+PGSIZE) = 'b';
    if(munmap(p+PGSIZE, PGSIZE) == -1)
      err("munmap");
    // this should cause a fatal fault
    printf("*(p+PGSIZE) = %x\n", *(p+PGSIZE));
    exit(0);
  }
  int st = 0;
  wait(&st);
  if(st != -1)
    err("child #1 read unmapped memory");

  pid = fork();
  if(pid < 0) err("fork");
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
  *p = 'P';
  *(p+PGSIZE) = 'Q';

  if(munmap(p, PGSIZE) == -1)
    err("munmap");

  *(p+PGSIZE) = 'R';
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

  //NOTE: mmap() 2 pages, RW + MAP_SHARED (so we have no issue writing back if needed)
  //close(fd), and expect the mmap region continues to work.
  //Modify the first char of each of the two pages.
  //fork() a child proc. In the child proc, modify the first char of each of the two pages.
  //Then munmap() 1 page from p. Then tries to read the first page from p.
  //It expects the program to trigger a usertrap() fatal fault.
  //The child proc then exit().
  //The parent proc then modifies the first char of each page again.
  //It then munmap() the first page of p. Modify the first char of the 2nd page, and munmap() it.
  //Finally, it reads the file back from disk, and checks whether the modifications are preserved.

  //It is a pretty complicated test. Here are the summaries:
  //1) close(fd) should not impact mmap region already created
  //2) child proc should share the same VMA data
  //3) munmap() and exit() in the child proc should not impact the parent proc
  //4) RW + MAP_SHARED mode mmap should retain all changes made in the mmap region

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

There are a lot of material in the comments. But we can summarize the requirements in a few bullet points:

- We should have 2 syscalls, `sys_mmap()` and `sys_munmap()`.
- We should save the VMA data as an array (multiple mapping) in `struct proc` (parent and child have separate).
- `sys_mmap()` should lazy map and rely on `vmfault()` to call the mmap fault handler do the actual map/kalloc job.
- `sys_mmap()` should refuse to mmap RO file as RW+MAP_SHARED.
- `sys_mmap()` should not be impacted by `close()`, maybe we can solve this by incrementing the `ref`.
- `sys_munmap()` should be able to write back the whole file, without changing its size, for certain PROC and FLAGS combination.
- `sys_munmap()` should also unmap the requested mmap region.
- We should have a mmap fault handler, let's call it `mmapfault()`.
- `vmfault()` should have access to the VMA data so that it knows when to call `mmapfault()`.
- `mmapfault()` reads data from file to the mmap region.


### Incremental approach

Actually, I think the above analysis is not realistic. It is impossible for a newbie to do the analysis fully and write down the whole specification. A much easier approach is just to try to pass the first test, and then the second, and so on. We still need to modify or add new stuffs for each of the new tests, but this is more approachable to ordinary people.


#### Test 1.1

```C
void
mmap_test(void)
{
  int fd;
  int i;
  const char * const f = "mmap.dur";

  //
  // create a file with known content, map it into memory, check that
  // the mapped memory has the same bytes as originally written to the
  // file.
  //
  makefile(f);
  if ((fd = open(f, O_RDONLY)) == -1)
    err("open (1)");

  printf("test basic mmap\n");

  char *p = mmap(0, PGSIZE*2, PROT_READ, MAP_PRIVATE, fd, 0);
  if (p == MAP_FAILED)
    err("mmap (1)");
  _v1(p);
  if (munmap(p, PGSIZE*2) == -1)
    err("munmap (1)");

  printf("test basic mmap: OK\n");

  //NOTE: Test 1 - mmap 2 pages: PROT = RO, FLAGS = MAP_PRIVATE
  //_v1() then reads 2 pages -> expected behavior: reads without issue
  //munmap() then unmaps the 2 pages

```

Test 1.1 only requires the programmer to implement the skeleton of `sys_mmap()` and `sys_munmap()`, as well as the user land stub `mmap()` and `munmap()`. I'll take the advantage to implement `struct vma` and put it into `struct proc`.

```C
//proc.h
//Add the following code after enum procstate

//mmap:
struct vma
{
  //File backed VMA
  //I store struct file * instead of fd
  //TODO: Figure out if it's possible to store fd instead -> need to harden against close(fd)
  struct file *f;
  vaddr_t startua;
  //A copy of the original startua, because startua can be modified by munmap().
  //e.g. if the program unmaps the first page, startua should move to the second page.
  vaddr_t originalstartua;
  //len can be modified by munmap() as well, but we don't need to make a copy
  int len;
  int prot;
  int flags;
};

//Add this define before struct proc
#define MAXMMAP 16

//Add the following code at the end of struct proc
struct proc {
  //...
  
  //16 mmap regions, each backed by a struct file *
  struct vma fmap[MAXMMAP];
  int totalvma;
}

```

I put a hardcoded limit to the number of elements for the array fmap. For now 16 entries is good enough for the test. I don't know how production kernel mmap works, yet.

```C
//sysfile.c
//Add sys_mmap() and sys_munmap() at the end
char*
sys_mmap(void)
{
  int len;
  int prot;
  int flags;
  int fd;
  int offset;

  //Step 1: Read cli arguments
  //Ignore the first argument as it is always 0 in the tests
  argint(1, &len);
  argint(2, &prot);
  argint(3, &flags);
  argint(4, &fd);
  argint(5, &offset);

  //Step 2: Fetch the struct file *
  struct proc *p = myproc();
  struct file *f = p->ofile[fd];
  if (!f)
    panic("sys_mmap: fd already closed");

  //Step 3: Set file offset
  f->off = offset;

  //Step 4: Create a VMA object and slap it into the proc
  //Temporarily put the data into the first entry. We will implement a function that returns the index of the next empty entry.
  int lastfreevma = 0;
  struct vma fm;
  //My custom mmap flavor: each mmap region takes 1 GiB (Who needs more than 1 GiB?)
  //The first starts from MMAPSTART, the second from MMAPSTART + 1 GiB, etc.
  //#define MMAPSTART (TRAPFRAME - MAXMMAP * GiB)
  fm.startua = MMAPSTART + lastfreevma * GiB;
  //Save the original startua for full file writeback, as startua may change
  fm.originalstartua = fm.startua;
  fm.len = len;
  fm.prot = prot;
  fm.flags = flags;
  fm.f = f;

  p->fmap[lastfreevma] = fm;

  return (char*)fm.startua;
}

//#define uint64 vaddr_t
vaddr_t
sys_munmap(void)
{
  vaddr_t addr = 0;
  int len;
  int writeback = 0;

  //Step 1: Read cli arguments
  argaddr(0, &addr);
  argint(1, &len);
  vaddr_t baseaddr = PGROUNDDOWN(addr);

  //len should be aligned to PGSIZE, 
  //otherwise we have no idea what to do if it's half page
  ASSERT(len % PGSIZE == 0);

  //Step 2: Check which mmap region addr belongs to
  struct proc *p = myproc();
  //Hardcode to the first entry at the moment.
  //In a while we will implement a function to find the index
  int index = 0;

  //Step 3: Unmap the region
  uint64 npages = len / PGSIZE;
  uvmunmap(p->pagetable, baseaddr, npages, 1);

  //For now, just remove the VMA entry once munmap() is called.
  //In the future we need to move startua and len around based on the arguments of munmap().
  //The way we tell an entry is empty is by checking the struct file * against 0.
  p->fmap[index].f->ref -= 1;
  p->fmap[index].f = 0;
  p->fmap[index].flags = 0;
  p->fmap[index].len = 0;
  p->fmap[index].prot = 0;
  p->fmap[index].startua = 0;
  p->totalvma -= 1;

  return 0;
}
```


```C

  printf("test mmap private\n");

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

  // file should not have been modified.
  if((fd = open(f, O_RDONLY)) < 0) err("open");
  if(read(fd, buf, PGSIZE) != PGSIZE) err("read");
  if(buf[0] != 'A')
    err("write to MAP_PRIVATE was written to file");
  if(read(fd, buf, PGSIZE) != PGSIZE/2) err("read");
  if(buf[0] != 'A')
    err("write to MAP_PRIVATE was written to file");
  close(fd);

  printf("test mmap private: OK\n");

  //NOTE: Test 2 - mmap 2 pages: PROT = RW, FLAGS = MAP_PRIVATE
  //_v1(p) then reads 2 pages -> expected behavior: reads without issue
  //Then writes 'Z' to 2 pages, and munmap(), close(fd)
  //The expected behavior is that the file should NOT be overwritten with 'Z'
  //because FLAGS = MAP_PRIVATE

  printf("test mmap read-only\n");

  // check that mmap doesn't allow read/write mapping of a
  // file opened read-only.
  if ((fd = open(f, O_RDONLY)) == -1)
    err("open (2)");
  p = mmap(0, PGSIZE*2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p != MAP_FAILED)
    err("mmap (3)");
  if (close(fd) == -1)
    err("close (2)");

  printf("test mmap read-only: OK\n");

  //NOTE: Test 3 - mmap 2 pages: PROT = RW, FLAGS = MAP_SHARED
  //But the file is opened as RO. 
  //So the expected behavior is that mmap() should return -1

  printf("test mmap read/write\n");

  // check that mmap does allow read/write mapping of a
  // file opened read/write.
  if ((fd = open(f, O_RDWR)) == -1)
    err("open (3)");
  p = mmap(0, PGSIZE*3, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED)
    err("mmap (4)");
  if (close(fd) == -1)
    err("close (3)");

  // check that the mapping still works after close(fd).
  _v1(p);

  // write the mapped memory.
  for (i = 0; i < PGSIZE; i++)
    p[i] = 'B';
  for (i = PGSIZE; i < PGSIZE*2; i++)
    p[i] = 'C';

  // unmap just the first two of three pages of mapped memory.
  if (munmap(p, PGSIZE*2) == -1)
    err("munmap (3)");

  printf("test mmap read/write: OK\n");

  printf("test mmap dirty\n");

  // check that the writes to the mapped memory were
  // written to the file.
  if ((fd = open(f, O_RDONLY)) == -1)
    err("open (4)");
  int temp = read(fd, buf, PGSIZE);
  if(temp != PGSIZE)
    err("dirty read #1");
  for (i = 0; i < PGSIZE; i++){
    if (buf[i] != 'B')
      err("file page 0 does not contain modifications");
  }
  temp = read(fd, buf, PGSIZE);
  if(temp != PGSIZE/2)
  {
    printf("mmaptest: read returns 0x%x\n", temp);
    err("dirty read #2");
  }
  for (i = 0; i < PGSIZE/2; i++){
    if (buf[i] != 'C')
      err("file page 1 does not contain modifications");
  }
  if (close(fd) == -1)
    err("close (4)");

  printf("test mmap dirty: OK\n");

  //NOTE: Test 4 - mmap 3 pages: PROT = RW, FLAGS = MAP_SHARED
  //Then close(fd). Then _v1(p) reads the first 2 pages.
  //Then write 'B' to the first page, and 'C' into the second page.
  //Then munmap() the first 2 pages.
  //Then read() and checks whether the first page is all 'B'.
  //Note that read() retains the offset, so the second read() actually starts from the beginning of the 2nd page,
  //which read just 0.5 pages.
  //Then continue read() and checks whether the first half of the second page is all 'C'
  //Then close(fd)
  //This is one of the more difficult tests, and we can summarize the expectations as:
  //1) If PROT=RW, FLAGS=MAP_SHARED, we should write back any changes made to the mmap region.
  //2) However, we should never "extend" the file in write back.
  //You can see that the program checks whether the first read() reads 1 page and the second reads 0.5 pages.
  //If we extend the file, then the second read() is going to read a full 1 page.
  //(The lab hints note that we can always write back the full file without worrying about the dirty bit)

  printf("test not-mapped unmap\n");

  // unmap the rest of the mapped memory.
  if (munmap(p+PGSIZE*2, PGSIZE) == -1)
    err("munmap (4)");

  printf("test not-mapped unmap: OK\n");

  //NOTE: Test 5 - munmap() the 3rd page (recall that we mmap 3 pages ^?)
  //Not much to say here. The expected result is that we should be able to
  //munmap() any region mapped by mmap()

  printf("test lazy access\n");

  if(unlink(f) != 0) err("unlink");
  makefile(f);

  if ((fd = open(f, O_RDWR)) == -1)
    err("open");
  p = mmap(0, PGSIZE*2, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED)
    err("mmap");
  close(fd);

  // mmap() should not have read the file at this point,
  // so that the file modification we're about to make
  // ought to be visible to a subsequent read of the
  // mapped memory.

  if((fd = open(f, O_RDWR)) == -1)
    err("open");
  if(write(fd, "m", 1) != 1)
    err("write");
  close(fd);

  //Immediately traps into vmfault() because p is lazily allocated
  if(*p != 'm')
    err("read was not lazy");

  if(munmap(p, PGSIZE*2) == -1)
    err("munmap");

  printf("test lazy access: OK\n");

  //NOTE: Test 6 - The program makes the file anew.
  //Then it mmap() 2 pages: PROT = RW, FLAGS = MAP_SHARED
  //Then close(fd).
  //Then write() 'm' (just 1 byte) into the file.
  //Then reads from *p and checks whether it is 'm'.
  //Then munmap() the 2 pages.
  //The expectation is: mmap() should never pre-load the file into the mapped pages.
  //Instead, the kernel relies on fault handlers to load the file once needed.
  //The code that triggers the handler is if(*p != 'm').

  printf("test mmap two files\n");

  //
  // mmap two different files at the same time.
  //
  int fd1;
  if((fd1 = open("mmap1", O_RDWR|O_CREATE)) < 0)
    err("open (5)");
  if(write(fd1, "12345", 5) != 5)
    err("write (1)");
  char *p1 = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd1, 0);
  if(p1 == MAP_FAILED)
    err("mmap (5)");
  if (close(fd1) == -1)
    err("close (5)");
  if (unlink("mmap1") == -1)
    err("unlink (1)");

  int fd2;
  if((fd2 = open("mmap2", O_RDWR|O_CREATE)) < 0)
    err("open (6)");
  if(write(fd2, "67890", 5) != 5)
    err("write (2)");
  char *p2 = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd2, 0);
  if(p2 == MAP_FAILED)
    err("mmap (6)");
  if (close(fd2) == -1)
    err("close (6)");
  if (unlink("mmap2") == -1)
    err("unlink (2)");

  if(memcmp(p1, "12345", 5) != 0)
    err("mmap1 mismatch");
  if(memcmp(p2, "67890", 5) != 0)
    err("mmap2 mismatch");

  if (munmap(p1, PGSIZE) == -1)
    err("munmap (5)");
  if(memcmp(p2, "67890", 5) != 0)
    err("mmap2 mismatch (2)");
  if (munmap(p2, PGSIZE) == -1)
    err("munmap (6)");

  printf("test mmap two files: OK\n");

  //NOTE: Test 7 - mmap 1 page for two files
  //This tests whether the kernel knows how to handle multiple VMAs.
  //If the programmer only implements one VMA, then this test fails.
}
```