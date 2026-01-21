## Postmortem of mmap lab

### Introduction

This is the last lab of the series, and touches both memory management and file system, so I figured it worths writing down what I learned, and more importantly, what I *would* do if given a restart.

It is from my experience working on the labs that I learned that kernel programming, and system programming, however elementary they may look like, post significant taxes on the programmer's patience and obssession to correctness.

- Reading the understanding the tests
- Figuring out specification for `mmap` and `munmap`
- Writing the implementation

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

Proceed by running `make qemu` and we see that our "implementation" passes the first test:

```bash
$ mmaptest
test basic mmap
test basic mmap: OK
test mmap private
panic: fileclose
```

#### Test 1.2 - test mmap private

```C

  printf("test mmap private\n");

  //mmap 2 pages. Note that the file is only 1.5 pages. This means we can over-mmap.
  p = mmap(0, PGSIZE*2, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (p == MAP_FAILED)
    err("mmap (2)");
  //close(fd) should not panic as shown in the ^ log.
  //This means mmap() should increment f->ref, otherwise f->ref is 0 and fileclose() panics.
  //Note that close(fd) always puts p->ofile[fd] to 0. This makes the next call of close(fd) returns -1.
  if (close(fd) == -1)
    err("close (1)");
  //p is mmaped, but never allocated/mapped, so _v1(p) triggers vmfault() with scause=13 (load page fault)
  _v1(p);
  for (i = 0; i < PGSIZE*2; i++)
    p[i] = 'Z';
  if (munmap(p, PGSIZE*2) == -1)
    err("munmap (2)");
  //Since fd was already closed ^, this close(fd) should return -1, 
  //as argfd() call is not able to locate ofile[fd], i.e. f=myproc()->ofile[fd]) == 0.
  //This is probably why it is not tested against -1.
  //
  //Additional note: if you track f->ref till this line, it should be 0,
  //which means if somehow sys_close() manages to run fileclose(), it panics.
  close(fd);

  // file should not have been modified, because mmap region is MAP_PRIVATE.
  //The rest of the test simply checks whether the file has been modified.
  if((fd = open(f, O_RDONLY)) < 0) err("open");
  if(read(fd, buf, PGSIZE) != PGSIZE) err("read");
  if(buf[0] != 'A')
    err("write to MAP_PRIVATE was written to file");
  if(read(fd, buf, PGSIZE) != PGSIZE/2) err("read");
  if(buf[0] != 'A')
    err("write to MAP_PRIVATE was written to file");
  close(fd);

  printf("test mmap private: OK\n");
```

Test 1.2 calls `_v1(p)`, which reads bytes from the mmap region. Recall that we never kalloc()/mappages() in mmap(), i.e. we simply lazily mmap 2 pages of file to `p` by TELLING the kernel -- technically, by inserting a new entry into `p->fmap[]`. The CPU then calls `usertrap()` (because `mmaptest.c` is a user land program, and because the xv6 kernel puts the address of `usertrap()` into `stvec` when running user code) with `scause` as 13 (because this is a LOAD PAGE FAULT). `usertrap()` then calls `vmfault()`, but there is no code to deal with this situation in `vmfault()`, so this leaves us to implement the functionality.

What I did (perhaps not the cleanest option, from hindsight) is to make a new kernel function in `vm.c` -- `mmapfault()`, and takes over from `vmfault()` if the address is within the range of a fmap entry.

```C
//vm.c, in vmfault(), under struct proc *p = myproc();

  //mmap: check if va is part of mmap addr.
  //findmapwithin() is to be implemented shortly.
  int index = findmmapwithin(p, va);
  if (index >= 0)
    return mmapfault(pagetable, va, index, read);

//vm.c, add a new function at the end.
//Grab the index of the element of p->fmap array that CONTAINS addr
int 
findmmapwithin(struct proc * p, vaddr_t addr)
{
  int i = 0;
  for (; i < MAXMMAP; i++)
  {
    vaddr_t startua = p->fmap[i].startua;
    vaddr_t endua = p->fmap[i].startua + p->fmap[i].len;
    if ((addr >= startua) && (addr < endua))
      return i;
  }
  return -1;
}

//vm.c, add a new function at the end.
//vaddr_t and paddr_t are both uint64.
uint64
mmapfault(pagetable_t pagetable, vaddr_t va, int fmapidx, int read)
{
  //For read fault, should load file into va
  //e.g. mmap region from 0x4000 to 0xA000, a total of 6 pages
  //va = 0x5400, then the page starts from 0x5000 to 0x6000
  vaddr_t baseva = PGROUNDDOWN(va);
  struct proc *p = myproc();

  struct file *f = p->fmap[fmapidx].f;
  if (!f)
    panic("mmapfault: file is NULL");

  paddr_t mem = (uint64)kalloc();
  if (mem == 0)
    return 0;
  memset((void *)mem, 0, PGSIZE);
  //I think we have to enable PTE_W even for readonly mmap regions
  //becuase we need to write into it for mmap.
  //RO/RW should be implemented by looking at prot and flags.
  //Please note that dirty bit is not implemented throughout this lab.
  if (mappages(pagetable, baseva, PGSIZE, mem, PTE_R|PTE_U|PTE_W) != 0)
  {
    kfree((void *)mem);
    return 0;
  }

  //For now we simply slap on a fileread() which works for this specific test.
  //We will see in the near future that this no longer works when the tests become more sophiscated.
  int bytesread = fileread(f, baseva, PGSIZE);
  if (bytesread <= 0)
    panic("mmapfault: fileread failed!");

  return mem;
}

```

Once the above code has been added, you should see the following printed message after running `mmaptest` again:

```
$ mmaptest
test basic mmap
test basic mmap: OK
test mmap private
test mmap private: OK
test mmap read-only
mmaptest failure: mmap (3), pid=3
```

#### Test 1.3 - test mmap read-only

```C

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
```

This is the second easiest test. Basically it says, if file is opened as RO, `mmap()` should NOT be able to mmap the region as writable. We only need to add one check in `mmapfault()`:

```C
  //sysfile.c, sys_mmap(), above f->off = offset;
  //No R/W mapping for a file opened RO
  if ((prot & PROT_WRITE) && (f->writable == 0) && (flags & MAP_SHARED))
    return (char *) -1;
```

Once we run the program, we see that it passes this test, and the next one as well.

```
$ mmaptest
test basic mmap
test basic mmap: OK
test mmap private
test mmap private: OK
test mmap read-only
test mmap read-only: OK
test mmap read/write
test mmap read/write: OK
test mmap dirty
mmaptest failure: file page 0 does not contain modifications, pid=3
```

#### Test 1.4 - test mmap read/write

```C

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

```

Test 1.4 doesn't have any new materials. However, technically the next test relies on the code in this test, so we will need to come back to this one shortly. I'll paste the output again. Note that test mmap dirty failed to pass.

```
$ mmaptest
test basic mmap
test basic mmap: OK
test mmap private
test mmap private: OK
test mmap read-only
test mmap read-only: OK
test mmap read/write
test mmap read/write: OK
test mmap dirty
mmaptest failure: file page 0 does not contain modifications, pid=3
```

#### Test 1.5 - test mmap dirty

```C

  printf("test mmap dirty\n");

  // check that the writes to the mapped memory were
  // written to the file.
  if ((fd = open(f, O_RDONLY)) == -1)
    err("open (4)");
  int temp = read(fd, buf, PGSIZE);
  if(temp != PGSIZE)
    err("dirty read #1");
  //This checks whether the previous write into the mmap region
  //has been written back to the file.
  for (i = 0; i < PGSIZE; i++){
    if (buf[i] != 'B')
      err("file page 0 does not contain modifications");
  }
  temp = read(fd, buf, PGSIZE);
  //This checks whether we wrote back the correct length of bytes
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

```

The most important thing about this part is to implement the write back. There are a few places that I could implement it, but I chose to implement it in `munmap()`. It does have a few advantages.

- It passes all tests. Eventually none of tests failed.
- It is the last point that we have to do a write back. Otherwise we lose the mapping information and can never do the writeback again.
- `munmap()` is not called fairly often, and since I only intend to implement full file writeback, it is faster.

```C
//sysfile.c, in sys_munmap(), here is the full source code because there is a lot of changes.
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

  //Step 2: Check which mmap region addr belongs to
  struct proc *p = myproc();
  //Hardcode to the first entry at the moment.
  //In a while we will implement a function to find the index
  int index = 0;

  //Do we need to writeback?
  //Has to have PROT_WRITE as well as MAP_SHARED, and file is writable
  if ((
    p->fmap[index].prot & PROT_WRITE) && 
    (p->fmap[index].flags & MAP_SHARED) &&
    p->fmap[index].f->writable
  )
    writeback = 1;

  //Step 3: Write back
  if (writeback)
  {
    struct file *f = p->fmap[index].f;
    if (!(f->writable))
      panic("sys_munmap: Supposed to writeback but f is not writable");

    //NOTE: _v1(p) calls `fileread()` which modifies f->off.
    //So we need to reset it to 0 once we decide to write back.
    f->off = 0;
    //NOTE: `filewrite()` is not supposed to increment file size.
    //So we need to fetch the size and write back the whole file.
    int ret = filewrite(f, p->fmap[index].originalstartua, f->ip->size);
    if (ret < 0)
      panic("sys_munmap: file writeback failed");
  }

  //Step 4: Unmap the region
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

The key of the writeback, is to write back the whole file without changing its size. To do that, we need two things -- the original startua, which is the starting point of the mmap region, and the size of the file, which is stored in `f->ip->size`. I used to write:

```C
//sys_munmap(), wrong writeback
    //instead of f->ip->size, we write back len, the second argument of the function
    int ret = filewrite(f, p->fmap[index].originalstartua, len);
```

But this is wrong and breaks the following test. When debugging in `gdb` I found that instead of reading PGSIZE/2 it reads a full page. Why? Because `filewrite()` calls `writei()`, and then updates `f->off` by adding `r`. Then `writei()` updates `ip->size` based on `off`. So eventually I incremented the file size from 1.5 pages to 2 pages. And then `read()` reads 2 pages instead of 1.5 pages, which breaks the test.

```C
  temp = read(fd, buf, PGSIZE);
  //This checks whether we wrote back the correct length of bytes
  if(temp != PGSIZE/2)
  {
    printf("mmaptest: read returns 0x%x\n", temp);
    err("dirty read #2");
  }
```

Here is the output afterwards. We managed to pass the mmap dirty test but fails the not-mapped unmap test.

```
$ mmaptest
test basic mmap
test basic mmap: OK
test mmap private
test mmap private: OK
test mmap read-only
test mmap read-only: OK
test mmap read/write
test mmap read/write: OK
test mmap dirty
test mmap dirty: OK
test not-mapped unmap
scause=0xd sepc=0x80004c7a stval=0x4
panic: kerneltrap
```

```C

  printf("test not-mapped unmap\n");

  // unmap the rest of the mapped memory.
  if (munmap(p+PGSIZE*2, PGSIZE) == -1)
    err("munmap (4)");

  printf("test not-mapped unmap: OK\n");

  //NOTE: Test 5 - munmap() the 3rd page (recall that we mmap 3 pages ^?)
  //Not much to say here. The expected result is that we should be able to
  //munmap() any region mapped by mmap()
```

The reason this test fails is because we naively remove the fmap entry once `sys_munmap()` is called. But for this 3 pages mmap region, it was actually unmmapped by 2 `sys_munmap()` calls. So in our naive implementation, the first `munmap()` already removes the entry, so the second one could not find the entry, thus it triggers a kernel panic (cannot read array index).

```C
  // unmap just the first two of three pages of mapped memory.
  if (munmap(p, PGSIZE*2) == -1)
    err("munmap (3)");

  //...rest of the code

  // unmap the rest of the mapped memory.
  if (munmap(p+PGSIZE*2, PGSIZE) == -1)
    err("munmap (4)");
```

So now we need to think how should we deal with this. The obvious answer is: we don't remove the entry, until the whole mmap region has been unmapped. It is not easy to implement for all cases. For example, you could have a 3 page mmap region, and then the 2nd page is unmapped. Now you have to split one 3 page region into two 1 page region. This probably involes some data structure more complicated than a simple array. Fortunately, none of the tests grills us on this kind of cases. So we can get away by saving the original `endua` (which is `startua`+`len`), moving `startua` and `len` around for each `sys_munmap()` call, and removing the entry only when the new `startua` >= `endua`.

```C
//sysfile.c, sys_munmap(), under writeback = 1

  vaddr_t newstartua, endua = 0;
  if (baseaddr <= p->fmap[index].startua)
  {
    //e.g. munmap(p, PGSIZE * 2);
    //baseaddr should never < startua, but just in case
    newstartua = p->fmap[index].startua + len;
    endua = p->fmap[index].startua + p->fmap[index].len;
  }
  else
  {
    //e.g. munmap(p+PGSIZE, PGSIZE);
    //assuming munmap() never breaks the region into multiple parts
    //i.e. it either unmaps some mem in the front, or in the back
    newstartua = p->fmap[index].startua;
    endua = baseaddr;
  }

//same file, between uvmunmap(p->pagetable, baseaddr, npages, 1); and return 0;

  //If we already unmapped the whole mmap region, remove the entry.
  if (newstartua >= endua)
  {
    DPRINTF("sys_munmap: removed fmap entry %d\n", index);
    p->fmap[index].f->ref -= 1;
    p->fmap[index].f = 0;
    p->fmap[index].flags = 0;
    p->fmap[index].len = 0;
    p->fmap[index].prot = 0;
    p->fmap[index].startua = 0;
    p->totalvma -= 1;
  }
  //Otherwise, simply increment startua
  else
  {
    p->fmap[index].startua = newstartua;
    p->fmap[index].len -= len;
  }
```

After these two changes, we managed to get a new error:

```
$ mmaptest
test basic mmap
test basic mmap: OK
test mmap private
test mmap private: OK
test mmap read-only
test mmap read-only: OK
test mmap read/write
test mmap read/write: OK
test mmap dirty
test mmap dirty: OK
test not-mapped unmap
panic: sys_munmap: file writeback failed
```

If we go back to `mmaptest.c`, we see that it is trying to execute `if (munmap(p+PGSIZE*2, PGSIZE) == -1)`. But the file is only 1.5 pages long, so there is no point to write back. I added a check for `writeback` and it worked.

```C
//sysfile.c, sys_munmap()

  if ((
    p->fmap[index].prot & PROT_WRITE) && 
    (p->fmap[index].flags & MAP_SHARED) &&
    p->fmap[index].f->writable &&
    p->fmap[index].originalstartua + p->fmap[index].f->ip->size > addr
  )
    writeback = 1;
```

#### Test 1.6 - test lazy access

```C

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

```

This test ran into a deadlock in `sys_munmap()`. The reason is that, the test program checked for `*p`, in `if(*p != 'm')`. So it triggers `mmapfault()` to load the first page of the file. It doesn't load the second page because it doesn't need. But when we write back in `sys_munmap()`, it tries to write back both pages, so this triggers another `mmapfault()`, which tries to load by calling `fileread()`. Both `filewrite()` (called by `sys_munmap()`) and `fileread()` need to lock the inode, thus caused the deadlock.

The best way to solve this issue, is to implement the dirty bit -- writing back a page never mapped is very weird in its own sense. However, I'm a bit lazy, so I created a special version of `filewrite()`, `writei()` and `copyin()` -- `filewriteback()`, `writebacki()` and `copyinback()`. The only real difference is that in `copyinback()` if `pa0` is 0 we then ignore it and do not call `vmfault()`. This is probably not the best idea, but it works, meh. I'll implement the dirty bit in a different branch.

#### Test 1.7 - test mmap two files

```C

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

Remember that in both `sys_mmap()` and `sys_munmap()` that we hardcoded the fmap entry index to 0? Now we have two files, so this test fails.

```
$ mmaptest
test basic mmap
test basic mmap: OK
test mmap private
test mmap private: OK
test mmap read-only
test mmap read-only: OK
test mmap read/write
test mmap read/write: OK
test mmap dirty
test mmap dirty: OK
test not-mapped unmap
test not-mapped unmap: OK
test lazy access
test lazy access: OK
test mmap two files
mmaptest failure: mmap1 mismatch, pid=3
```

Now, if we want to modify the code to use multiple entries in the fmap array, we need to write some helper function to locate an empty entry. We also need some code to find the right entry given a virtual address. This results in 3 functions, one function `fmmap()` in `sysfile.c` which finds an empty entry by looking for an empty `struct file *` pointer, and two functions `findmmapbase()` and `findmmapwithin()` In `vm.c` which finds the entry index given a virtual address.

```C
//sysfile.c
//Grab the index of first free element of p->fmap array
static int 
ffmmap(struct proc * p)
{
  int i = 0;
  for (; i < MAXMMAP; i++)
  {
    if (!(p->fmap[i].f))
      return i;
  }
  return -1;
}

//vm.c, under vmfault(), or wherever you prefer
//Grab the index of the element of p->fmap array whose startva MATCHES addr
int 
findmmapbase(struct proc * p, vaddr_t startua)
{
  int i = 0;
  for (; i < MAXMMAP; i++)
  {
    if (startua == p->fmap[i].startua)
      return i;
  }
  return -1;
}

//Grab the index of the element of p->fmap array that CONTAINS addr
int 
findmmapwithin(struct proc * p, vaddr_t addr)
{
  int i = 0;
  for (; i < MAXMMAP; i++)
  {
    vaddr_t startua = p->fmap[i].startua;
    vaddr_t endua = p->fmap[i].startua + p->fmap[i].len;
    if ((addr >= startua) && (addr < endua))
      return i;
  }
  return -1;
}

//And don't forget the plumbing work...
//defs.h, under the following two lines...
//uint64          vmfault(pagetable_t, uint64, int);
//uint64          mmapfault(pagetable_t pagetable, uint64 va, int fmapidx, int read);
int             findmmapbase(struct proc * p, uint64 startua);
int             findmmapwithin(struct proc * p, uint64 addr);
```

The code should be self-explanatory, so I'll proceed to use them in `sys_mmap()`:

```C
//sysfile.c, at the top to add a declaration:
static int ffmmap(struct proc * p);

//sysfile.c, sys_mmap(), change Step 4 (between `f->off = offset;` and `struct vma fm;` ) to:

  //Step 4: Create a VMA object and slap it into the proc
  //Check if fmap is full
  if (p->totalvma == MAXMMAP)
    return 0;
  int lastfreevma = ffmmap(p);

//sysfile.c, sys_munmap(), change Step 2 to:

  //Step 2: Check which mmap region addr belongs to
  struct proc *p = myproc();

  int index = findmmapwithin(p, baseaddr);
  if (index == -1)
  {
    printf("Out of range 0x%lx\n", baseaddr);
    return -1;
  }

```

Now we have passed the first test.

```
$ mmaptest
test basic mmap
test basic mmap: OK
test mmap private
test mmap private: OK
test mmap read-only
test mmap read-only: OK
test mmap read/write
test mmap read/write: OK
test mmap dirty
test mmap dirty: OK
test not-mapped unmap
test not-mapped unmap: OK
test lazy access
test lazy access: OK
test mmap two files
test mmap two files: OK
```

#### Test 2 - fork test

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
}
```

The first observation is that the child proc should have the same fmap entries as its parent. In the test program, the second page starting from `p1` was read from the parent proc first. This calls `vmfault()` which calls `mmapfault()` to allocate a phsycial page and map the virtual address to it by `usertrap()`.

```C
  // read just 2nd page.
  if(*(p1+PGSIZE) != 'A')
    err("fork mismatch (1)");
```

Then the parent proc forks, and its child proc calls `_v1()`. `_v1()` checks both pages -- it checks the first page first, so again `vmfault()` is called to call `mmapfault()` allocate a physical page and map the cirtual address to it by `usertrap()`.

```C
  if (pid == 0) {
    //_v1() triggers mmapfault(), which looks at p->fmap.
    //So child proc needs to have the same fmap as its parent's.
    _v1(p1);
    if (munmap(p1, PGSIZE) == -1) // just the first page
      err("munmap (7)");
    printf("pid: %d\n", getpid());
    exit(0); // tell the parent that the mapping looks OK.
  }
```

I took the easy route: a direct copy of the fmap entries from the parent proc to the child proc.

```C
//proc.c, kfork(), between `np->sz = p->sz;` and `*(np->trapframe) = *(p->trapframe);`
  
  //mmap: Copy vma to child
  np->totalvma = p->totalvma;
  for (int i = 0; i < MAXMMAP; i++)
  {
    np->fmap[i] = p->fmap[i];
    //printf("kfork: file pointer %p\n", np->fmap[i].f);
  }
```

Let's run the test again. Surprisingly, it fails the `_v1()` check. Why doesn't it work?

```
test fork
mismatch at 2048, wanted 'A', got 0x0
mmaptest failure: v1 mismatch (1), pid=4
fork_test failed
```

Let's take a second look at how `mmapfault()` reads the file into the mmap region. In the first call (that the program tries to read the second page), `va` is `p1+PGSIZE`, but we didn't do anything to change the offset of the file, so `fileread()` simply reads the first page instead, and incremented `f->off` by a page. We were lucky to pass this part of the test because the whole file was written in 'A', so reading the wrong page was not a big deal. In fact, I wrote a different test that loads the first page with 'A' and the second with 'B', and immediately this test failed.

However, we didn't manage to pass the `_v1(p1)` test from the child proc. `_v1()` checks the first page first, which has not been mapped yet, so it calls out to `mmapfault()` again. Recall that the offset is already at the beginning of the second page? Yeah so `mmapfault()` calls `fileread()` which happily reads the second page. Sadly our file is only 1.5 pages long, so half of the second page is just 0x0, exactly as shown in the above error message.

```C
uint64
mmapfault(pagetable_t pagetable, vaddr_t va, int fmapidx, int read)
{
  // previous code...
  // mappage() call

  int bytesread = fileread(f, baseva, PGSIZE);
  if (bytesread <= 0)
    panic("mmapfault: fileread failed!");

  return mem;
}
```

So to correct the behavior, we need to tell `fileread()` to load any page, not to blindly start from the first page. However, I felt this would require a lot of modification to `fileread()`, so instead I wrote a bit more code and call `readi()` instead:

```C
//sysfile.c, `sys_munmap()`, replace the line `int bytesread = fileread(f, baseva, PGSIZE);` and below with:

  //Similar to fileread() but with an offset.
  //What if user program does NOT read in order?
  //We should be able to get base va addr from fmap
  vaddr_t basefmava = p->fmap[fmapidx].startua;
  //Get offset for readi()
  int vaoff = baseva - basefmava;
  //Can't use fileread() directly because it doesn't have an argument for offset
  ilock(f->ip);
  int bytesread = readi(f->ip, 1, baseva, vaoff, PGSIZE);
  //Do not increment the offset as in fileread(),
  //because offset = diff between startua and baseva
  iunlock(f->ip);
  if (bytesread <= 0)
    panic("mmapfault: fileread failed!");

  return mem;
```

I don't increment `f->off` as in `fileread()`, and the reason of this is that it is more flexible. If the program somehow decides to read the 3rd page first, and then jumps to read the 10th page, each time we only need to calculate the offset based on the original `startua`. We simply don't use `f->off` here. If the program sequentially reads, say, from the 1st page to the last page, it is also OK -- `mmapvault()` is going to be called for each page, and each time it simply calculates a new offset. Again there is no need to use `f->off` for tracking.

Once we made the ^ improvement, all tests passed, including the 3rd one `more_test()`:

```
$ mmaptest
test basic mmap
test basic mmap: OK
test mmap private
test mmap private: OK
test mmap read-only
test mmap read-only: OK
test mmap read/write
test mmap read/write: OK
test mmap dirty
test mmap dirty: OK
test not-mapped unmap
test not-mapped unmap: OK
test lazy access
test lazy access: OK
test mmap two files
test mmap two files: OK
test fork
pid: 4
test fork: OK
test munmap prevents access
usertrap(): unexpected scause 0xd pid=5
            sepc=0xbb2 stval=0x3c7ffff000
usertrap(): unexpected scause 0xd pid=6
            sepc=0xc3a stval=0x3c7fffe000
test munmap prevents access: OK
test writes to read-only mapped memory
usertrap(): unexpected scause 0xf pid=7
            sepc=0xd82 stval=0x3c7fffe000
test writes to read-only mapped memory: OK
mmaptest: all tests succeeded
```