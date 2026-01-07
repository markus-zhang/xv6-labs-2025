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
// static int ffmmap(struct proc * p);

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

// char*
// sys_mmap(void)
// {
//   // void *addr = 0;
//   int len;
//   int prot;
//   int flags;
//   int fd;
//   int offset;

//   //Step 1: Read cli arguments
//   //argaddr(0, addr);
//   argint(1, &len);
//   argint(2, &prot);
//   argint(3, &flags);
//   argint(4, &fd);
//   argint(5, &offset);

//   // DPRINTF("sys_mmap: addr is %p\n", addr);
//   DPRINTF("sys_mmap: len is %d\n", len);
//   DPRINTF("sys_mmap: prot is %d\n", prot);
//   DPRINTF("sys_mmap: flags is %d\n", flags);
//   DPRINTF("sys_mmap: fd is %d\n", fd);
//   DPRINTF("sys_mmap: offset is %d\n", offset);

//   //Step 2: Lazy allocate len/PGSIZE pages for mmap
//   struct proc *p = myproc();
//   DPRINTF("sys_mmap: max proc va is: %ld\n", p->sz);
//   struct file *f = p->ofile[fd];
//   if (!f)
//     panic("sys_mmap: fd already closed");

//   //NOTE: Probably should set offset too, because previous readi() moves offset
//   f->off = offset;

//   //No R/W mapping for a file opened RO
//   //Kinda complicated, I created the logic from reading mmaptest.c
//   //I don't quite understand it TBH
//   if ((prot & PROT_WRITE) && (f->writable == 0) && (flags & MAP_SHARED))
//     return (char *) -1;

//   //Do not touch p->sz, leave it to regular memory allocation/deallocation
//   //uint64 oldsz = p->sz;
//   //p->sz += len;
//   //DPRINTF("sys_mmap: oldsz %p - newsz %p\n", (void *)oldsz, (void *)(p->sz));

//   //Check if fmap is full
//   if (p->totalvma == MAXMMAP)
//     return 0;

//   int lastfreevma = ffmmap(p);
//   //Step 3: Mark in a special place in proc
//   struct vma fm;
//   //NOTE: Each mmap region takes 1 GiB
//   //The first starts from MMAPSTART, the second from MMAPSTART + 1 GiB, etc.
//   fm.startua = MMAPSTART + lastfreevma * GiB;
//   //Save the original startua for full file writeback,
//   //as startua may change
//   fm.originalstartua = fm.startua;
//   fm.len = len;
//   fm.prot = prot;
//   fm.flags = flags;
//   //NOTE: It's very difficult to find pathname from f or fd
//   fm.f = f;

//   p->fmap[lastfreevma] = fm;
//   //Increment file ref so that fileclose() keeps the file open, I think
//   fm.f->ref += 1;
//   p->totalvma += 1;
//   // printf("sys_mmap: ref of fd %d file 0x%lx is %d\n", fd, (uint64)fm.f, fm.f->ref);

//   //We did not allocate/mappage,
//   //so next time the program tries to access these pages,
//   //it should tirgger vmfault(),which calls mmapfault() first

//   //mmap starts from a specifc region ourside of "ordinary" memory allocation
//   DPRINTF(
//     "sys_mmap: done in slot %d, mmap region starts from %p, len 0x%x\n", 
//     lastfreevma, (void *)fm.startua, fm.len
//   );
//   return (char*)fm.startua;
// }

// //Grab the index of first free element of p->fmap array
// static int 
// ffmmap(struct proc * p)
// {
//   int i = 0;
//   for (; i < MAXMMAP; i++)
//   {
//     if (!(p->fmap[i].f))
//       return i;
//   }
//   return -1;
// }

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
  f->ref += 1;

  return (char*)fm.startua;
}

// vaddr_t
// sys_munmap(void)
// {
//   vaddr_t addr = 0;
//   int len;
//   int writeback = 0;

//   //Step 1: Read cli arguments
//   argaddr(0, &addr);
//   argint(1, &len);
//   vaddr_t baseaddr = PGROUNDDOWN(addr);

//   //TODO: len should be aligned to PGSIZE, 
//   //otherwise we have no idea what to do if it's half page
//   ASSERT(len % PGSIZE == 0);

//   //Step 2: Check which mmap region addr belongs to
//   struct proc *p = myproc();
//   //TODO: This logic is wrong, sometimes we munmap e.g. the second page
//   //munmap(p+PGSIZE, PGSIZE), findmapbase() won't find the correct index
//   //check out more_test() for such requirement
//   // int index = findmmapbase(p, baseaddr);
//   int index = findmmapwithin(p, baseaddr);
//   if (index == -1)
//   {
//     printf("Out of range 0x%lx\n", baseaddr);
//     return -1;
//   }

//   //Step 4: Do we need to writeback?
//   //Has to have PROT_WRITE as well as MAP_SHARED
//   //TODO: Actually f->writable should also be non-zero
//   if ((
//     p->fmap[index].prot & PROT_WRITE) && 
//     (p->fmap[index].flags & MAP_SHARED) &&
//     p->fmap[index].f->writable
//   )
//     writeback = 1;

//   //What I can confirm from all test programs, are --
//   //1) len, the second argument of munmap(), is always PGSIZE aligned.
//   //2) tests never munmap() a middle page, to break the mmap region into 2.
//   //Point 2) is especially important as it impacts the vma data structure
//   vaddr_t newstartua, endua = 0;
//   if (baseaddr <= p->fmap[index].startua)
//   {
//     //e.g. munmap(p, PGSIZE * 2);
//     //baseaddr should never < startua, but just in case
//     newstartua = p->fmap[index].startua + len;
//     endua = p->fmap[index].startua + p->fmap[index].len;
//   }
//   else
//   {
//     //e.g. munmap(p+PGSIZE, PGSIZE);
//     //assuming munmap() never breaks the region into multiple parts
//     //i.e. it either unmaps some mem in the front, or in the back
//     newstartua = p->fmap[index].startua;
//     endua = baseaddr;
//   }

//   //NOTE: We have to write back here. We cannot use vmfault(),
//   //because s_cause() = 15 is not triggered by mmap.
//   //We save the original startua, and write back the whole file.
  
//   if (writeback)
//   {
//     struct file *f = p->fmap[index].f;
//     printf("sys_munmap: writing back 0x%x bytes for addr %p\n", f->ip->size, (void *)baseaddr);
//     if (!(f->writable))
//       panic("sys_munmap: Supposed to writeback but f is not writable");

//     //NOTE: _v1(p) calls `fileread()` which modifies f->off.
//     //So we need to reset it to 0 once we decide to write back.
//     f->off = 0;
//     //NOTE: `filewrite()` is not supposed to increment file size.
//     //So we need to fetch the size and write back the whole file.
//     // printf("file size: 0x%x\n", f->ip->size);
//     // int ret = filewriteback(f, baseaddr, f->ip->size);
//     int ret = filewriteback(f, p->fmap[index].originalstartua, f->ip->size);
//     if (ret < 0)
//       panic("sys_munmap: file writeback failed");
//     // int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
//     // int i = 0;
//     // int n = p->fmap[index].len;
//     // int r, ret = 0;
//     // while(i < n){
//     //   int n1 = n - i;
//     //   if(n1 > max)
//     //     n1 = max;
//     //
//     //   begin_op();
//     //   ilock(f->ip);
//     //   if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0)
//     //   {
//     //     printf("sys_munmap: offset is 0x%x\n", f->off);
//     //     f->off += r;
//     //   }
//     //   iunlock(f->ip);
//     //   end_op();
//     //
//     //   if(r != n1){
//     //     // error from writei
//     //     printf("sys_munmap: error from writei. r is 0x%x and n1 is 0x%x, max is 0x%x\n", r, n1, max);
//     //     break;
//     //   }
//     //   i += r;
//     // }
//     // ret = (i == n ? n : -1);
//     // printf("filewrite: returns 0x%x\n", ret);
//   }

//   //Step 5: Unmap by calling uvmunmap()
//   //Unlike writeback, where we are not supposed to over-write,
//   //unmap should unmap whatever the caller requests.
//   uint64 npages = len / PGSIZE;
//   uvmunmap(p->pagetable, baseaddr, npages, 1);

//   //We need to track which part of the mmap region is unmapped.
//   //And only when the WHOLE region has been unmapped that we reset the entry.
//   //Example: Say we have a mmap region of 3 pages (12KiB).
//   //The first call unmaps 1 page (the first page).
//   //Should I increment startua by 1 page as well? 
//   //The second call unmaps 2 pages. Only by now I remove the entry.

//   //If we already unmapped the whole mmap region, remove the entry.
//   if (newstartua >= endua)
//   {
//     DPRINTF("sys_munmap: removed fmap entry %d\n", index);
//     p->fmap[index].f->ref -= 1;
//     p->fmap[index].f = 0;
//     p->fmap[index].flags = 0;
//     p->fmap[index].len = 0;
//     p->fmap[index].prot = 0;
//     p->fmap[index].startua = 0;
//     p->totalvma -= 1;
//   }
//   //Otherwise, simply increment startua
//   else
//   {
//     p->fmap[index].startua = newstartua;
//     p->fmap[index].len -= len;
//   }

//   //We don't need to reduce f->ref because fileclose does that.

//   DPRINTF("sys_munmap: release 0x%x bytes at addr %p\n", len, (void *)baseaddr);
//   DPRINTF("fmap[%d]: startua 0x%lx with len 0x%x\n", index, newstartua, p->fmap[index].len);

//   return 0;
// }

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