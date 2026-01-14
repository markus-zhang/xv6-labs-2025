//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "debug.h"
#include "memlayout.h"

// static uint64 is_mmap(uint64 va);
static int ffmmap(struct proc * p);

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;
  
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  // int fd;
  // struct file *f;

  // if(argfd(0, &fd, &f) < 0)
  //   return -1;

  // //printf("sys_close: ref is %d\n", f->ref);
  // if(f->ref == 0)
  //   myproc()->ofile[fd] = 0;
  // fileclose(f);
  // //NOTE: mmap issue
  // //I noticed that everytime `fileread()` is called, f->off is moved,
  // //and as long as it's the same f, f->off retains the change.
  // //This breaks `mmapfault()` for the second test in mmaptest.c.
  // //LINK - user/mmaptest.c#fileoff_issue
  // //Reset f->off if we intend to close it, even if we don't due to f->ref
  // f->off = 0;
  // return 0;

  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0){
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if(dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if(type == T_DIR){
    // now that success is guaranteed:
    dp->nlink++;  // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

 fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if((argstr(0, path, MAXPATH)) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = kexec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

//mmap: replace mappages() for memory mapped files
//Record fd, starting VA and length to a struct in the proc
//Sample call: 
//char *p = mmap(0, PGSIZE*2, PROT_READ, MAP_PRIVATE, fd, 0);

char*
sys_mmap(void)
{
  // void *addr = 0;
  int len;
  int prot;
  int flags;
  int fd;
  int offset;

  //Step 1: Read cli arguments.
  //We ignore the first argument addr, default to 0.
  argint(1, &len);
  argint(2, &prot);
  argint(3, &flags);
  argint(4, &fd);
  argint(5, &offset);

  //In Linux, len can be anything but mmap() rounds it to PGSIZE.
  //Here we enforce it to be a multiple of PGSIZE.
  ASSERT((len > 0) && (len % PGSIZE == 0));
  ASSERT((offset >= 0) && (offset % PGSIZE == 0));

  DPRINTF("sys_mmap: len is 0x%x\n", len);
  DPRINTF("sys_mmap: prot is %d\n", prot);
  DPRINTF("sys_mmap: flags is %d\n", flags);
  DPRINTF("sys_mmap: fd is %d\n", fd);
  DPRINTF("sys_mmap: offset is 0x%x\n", offset);

  //Step 2: Lazy allocate len/PGSIZE pages for mmap
  struct proc *p = myproc();
  DPRINTF("sys_mmap: max proc va is: %ld\n", p->sz);
  struct file *f = p->ofile[fd];
  if (!f)
    panic("sys_mmap: fd already closed");

  //TODO: Evaluate whether we need this. We manually calculate offset during writeback.
  //f->off = offset;

  //No R/W + MAP_SHARED mapping for a file opened RO
  if ((prot & PROT_WRITE) && (f->writable == 0) && (flags & MAP_SHARED))
    return (char *) -1;

  //Check if fmap is full
  if (p->totalvma == MAXMMAP)
    return 0;

  //Step 3: Find an empty entry in fmap array and insert one.
  int lastfreevma = ffmmap(p);
  struct vma fm;
  //NOTE: Each mmap region takes 1 GiB in the proc virtual address space.
  //The first starts from MMAPSTART, the second from MMAPSTART + 1 GiB, etc.
  fm.startua = MMAPSTART + lastfreevma * GiB;
  //Save the original startua for full file writeback, as startua may change.
  fm.originalstartua = fm.startua;
  fm.len = len;
  fm.prot = prot;
  fm.flags = flags;
  //Store offset in VMA so every write can reference it.
  fm.offset = offset;
  //NOTE: sys_close() always sets fd to 0, so file-backed mmap needs to use struct file *.
  fm.f = f;

  p->fmap[lastfreevma] = fm;
  //Increment file ref so that fileclose() does not panic.
  fm.f->ref += 1;
  p->totalvma += 1;

  //NOTE: Lazy allocation -- We do not allocate/mappage in sys_mmap().
  //Instead, the first time the program tries to access these mmap pages,
  //the CPU executes usertrap() with scause = 13/15, which calls vmfault() to alloc/mappage.

  //As mentioned ^, mmap region starts from a specifc va ourside of "ordinary" memory allocation
  DPRINTF(
    "sys_mmap: done in slot %d, mmap region starts from %p, len 0x%x\n", 
    lastfreevma, (void *)fm.startua, fm.len
  );
  return (char*)fm.startua;
}

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

uint64
sys_munmap2(void)
{
  vaddr_t addr = 0;
  int len = 0;
  int writeback = 0;

  //Step 1: Read cli arguments and ASSERT().
  argaddr(0, &addr);
  argint(1, &len);

  if (len == 0)
    return 0;

  //We use findmmapwithin() so addr is not required to be aligned to PGSIZE.
  //TODO: However munmap() in Linux requires this.
  ASSERT(addr % PGSIZE == 0);
  vaddr_t baseaddr = PGROUNDDOWN(addr);

  //According to the test program, len should be aligned to PGSIZE, 
  //TODO: However munmap() in Linux does not require this.
  ASSERT(len % PGSIZE == 0);

  //Step 2: Check which mmap region addr belongs to.
  struct proc *p = myproc();

  int index = findmmapwithin(p, baseaddr);
  if (index == -1)
  {
    printf("Out of range 0x%lx\n", baseaddr);
    return -1;
  }

  //Step 2: Do we need to writeback?
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

  //Step 3: Calculate newstartua and endua based on which region the user wants to munmap.
  //1) len, the second argument of munmap(), is always PGSIZE aligned.
  //2) tests never munmap() a middle page, to break the mmap region into 2.
  //Point 2) is especially important as it impacts the vma data structure.
  vaddr_t newstartua, endua = 0;
  if (baseaddr <= p->fmap[index].startua)
  {
    //e.g. munmap(p, PGSIZE * 2);
    //baseaddr should never < startua, but just in case.
    newstartua = p->fmap[index].startua + len;
    endua = p->fmap[index].startua + p->fmap[index].len;
  }
  else
  {
    //e.g. munmap(p+PGSIZE, PGSIZE);
    //assuming munmap() never breaks the region into multiple parts.
    //i.e. it either unmaps some mem in the front, or in the back.
    newstartua = p->fmap[index].startua;
    endua = baseaddr;
  }

  //Step 3: Write back in demand.
  if (writeback)
  {
    struct file *f = p->fmap[index].f;
    //printf("sys_munmap: writing back 0x%x bytes for addr %p\n", f->ip->size, (void *)baseaddr);
    if (!(f->writable))
      panic("sys_munmap: Supposed to writeback but f is not writable");

    //_v1(p) calls `fileread()` which modifies f->off.
    //So we need to reset it to 0 once we decide to write back.
    f->off = 0;
    //NOTE: `filewrite()` is not supposed to increment file size.
    //So we need to fetch the size and write back the whole file.
    int ret = filewriteback(f, p->fmap[index].originalstartua, f->ip->size);
    if (ret < 0)
      panic("sys_munmap: file writeback failed");
  }

  //Step 3: Unmap the region
  uint64 npages = len / PGSIZE;
  uvmunmap(p->pagetable, baseaddr, npages, 1);

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
    p->fmap[index].originalstartua = 0;
    p->totalvma -= 1;
  }
  //Otherwise, simply increment startua
  else
  {
    p->fmap[index].startua = newstartua;
    p->fmap[index].len -= len;
  }

  DPRINTF("sys_munmap: release 0x%x bytes at addr %p\n", len, (void *)baseaddr);
  DPRINTF("fmap[%d]: startua 0x%lx with len 0x%x\n", index, newstartua, p->fmap[index].len);

  return 0;
}

uint64
sys_munmap(void)
{
  vaddr_t addr = 0;
  int len = 0;
  int writeback = 0;

  //Step 1: Read cli arguments and ASSERT().
  argaddr(0, &addr);
  argint(1, &len);

  if (len == 0)
    return 0;

  //We use findmmapwithin() so addr is not required to be aligned to PGSIZE.
  //TODO: However munmap() in Linux requires this.
  ASSERT(addr % PGSIZE == 0);
  vaddr_t baseaddr = PGROUNDDOWN(addr);

  //According to the test program, len should be aligned to PGSIZE, 
  //TODO: However munmap() in Linux does not require this.
  ASSERT(len % PGSIZE == 0);

  //Step 2: Check which mmap region addr belongs to.
  struct proc *p = myproc();

  int index = findmmapwithin(p, baseaddr);
  if (index == -1)
  {
    printf("Out of range 0x%lx\n", baseaddr);
    return -1;
  }

  //Step 2: Do we need to writeback?
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

  //Step 3: Calculate newstartua and endua based on which region the user wants to munmap.
  //1) len, the second argument of munmap(), is always PGSIZE aligned.
  //2) tests never munmap() a middle page, to break the mmap region into 2.
  //Point 2) is especially important as it impacts the vma data structure.
  vaddr_t newstartua, endua = 0;
  if (baseaddr <= p->fmap[index].startua)
  {
    //e.g. munmap(p, PGSIZE * 2);
    //baseaddr should never < startua, but just in case.
    newstartua = p->fmap[index].startua + len;
    endua = p->fmap[index].startua + p->fmap[index].len;
  }
  else
  {
    //e.g. munmap(p+PGSIZE, PGSIZE);
    //assuming munmap() never breaks the region into multiple parts.
    //i.e. it either unmaps some mem in the front, or in the back.
    newstartua = p->fmap[index].startua;
    endua = baseaddr;
  }

  //Step 3: Write back in demand.
  if (writeback)
  {
    struct file *f = p->fmap[index].f;
    //printf("sys_munmap: writing back 0x%x bytes for addr %p\n", f->ip->size, (void *)baseaddr);
    if (!(f->writable))
      panic("sys_munmap: Supposed to writeback but f is not writable");

    //Ex. munmap(p+PGSIZE, PGSIZE), and the whole mmap region has an offset,
    //which is saved in p->fmap[index].offset.
    //A second offset, which is the offset of the addr to the start of mmap region,
    //is calculated as offset = p+PGSIZE - p = PGSIZE.
    //Both offsets are then added to be applied to the file position.

    printf(
      "fmap offset 0x%x, addr 0x%lx, startua 0x%lx\n",
      p->fmap[index].offset, addr, p->fmap[index].startua
    );
    int offset = p->fmap[index].offset + (addr - p->fmap[index].startua);
    // int offset = p->fmap[index].offset + (addr - p->fmap[index].originalstartua);

    //In this iteration, still assume we write from offset to eof.
    //TODO: Why are we writing back full size, instead of len? Check the doc.
    int nbytes = f->ip->size - offset;
    //Given a `struct file *f`, `vaddr_t addr`, `uint64 offset` and `int n`, 
    //the function writes `nbytes` bytes from `addr` into the file `f`, 
    //starting from offset `offset`.
    printf(
      "mmapwrite: from addr 0x%lx, at offset 0x%x, for 0x%x bytes\n",
      addr, offset, nbytes
    );
    DPRINTF("original startua: 0x%lx\n", p->fmap[index].originalstartua);
    int ret = mmapwrite(f, addr, offset, nbytes);

    //NOTE: `filewrite()` is not supposed to increment file size.
    //So we need to fetch the size and write back the whole file.
    //int ret = filewriteback(f, p->fmap[index].originalstartua, f->ip->size);
    if (ret < 0)
      panic("sys_munmap: file writeback failed");
  }

  //Step 3: Unmap the region
  uint64 npages = len / PGSIZE;
  uvmunmap(p->pagetable, baseaddr, npages, 1);

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
    p->fmap[index].originalstartua = 0;
    p->totalvma -= 1;
  }
  //Otherwise, simply increment startua
  else
  {
    //NOTE: This implementation only supports head/tail unmap.
    //It does not support splitting the mmap region into 2 or more subregions.
    //If the tail is unmmapped, no need to update offset.
    //If the head is unmmapped, move offset to original offset + len
    if (baseaddr == p->fmap[index].startua)
      p->fmap[index].offset += len;

    p->fmap[index].startua = newstartua;
    p->fmap[index].len -= len;
  }

  DPRINTF("sys_munmap: release 0x%x bytes at addr %p\n", len, (void *)baseaddr);
  DPRINTF("fmap[%d]: startua 0x%lx with len 0x%x\n", index, newstartua, p->fmap[index].len);

  return 0;
}