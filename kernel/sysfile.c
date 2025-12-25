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
#include "buf.h"
#include "bio.h"
#include "debug.h"

static void list(struct inode *ipath, char *parent);

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

  //namei() needs to live in a transaction (begin_op <-> end_op).
  begin_op();
  //Fetch the inode of path old
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  //Every time we read or write an inode, we need to lock it first
  ilock(ip);
  //Hard link avoids directories to prevent endless recursion, need to figure out why
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  //Every time we modify an inode, we need to update the on-disk dinode.
  //Caller of iupdate() must hold ip->lock.
  iupdate(ip);
  iunlock(ip);

  //Find the inode of the parent path of new (and dump the filename to name)
  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  //dirlink() writes a new struct dirent (inum, name) into dp
  //If name already exists under dp, it returns -1
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

//NOTE - If anything bad happened, rollback (concept of transaction)
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

  //NOTE - nameiparent() copies the final path element into name if found
  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  //NOTE - If name already exists, returns ip
  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    //TODO - I don't understand this part
    iunlockput(ip);
    return 0;
  }

  //NOTE - If we didn't find name, create a new inode
  if((ip = ialloc(dp->dev, type)) == 0){
    iunlockput(dp);
    return 0;
  }

  //NOTE - We already held dp's lock. Now we are going to hold ip's lock.
  //Would it cause deadlock? No, because ip is new so no other process can lock it
  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  //ip was just allocated, so set nlink to 1
  ip->nlink = 1;
  //signal that we want to write the change to disk
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    //TODO: I need to figure out what happens if cyclic ref count...
    //I guess we wouldn't be able to recycle the inode
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

  //iunlock() + iput()
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

  //NOTE - Create a new file
  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  //NOTE - OK just open the file
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    //NOTE - ^ create() returns ip locked, but namei() doesn't, so we need to ilock(ip)
    ilock(ip);
    //TODO: Directories can only be read from, not written into, WHY?
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

  //NOTE - Allocate a new file and a new file descriptor
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

// uint64
// sys_write(void)
// {
//   struct file *f;
//   int n;
//   uint64 p;
  
//   argaddr(1, &p);
//   argint(2, &n);
//   if(argfd(0, 0, &f) < 0)
//     return -1;

//   return filewrite(f, p, n);
// }

uint64
sys_symlink(void)
{
  char path[MAXPATH];
  int fd;
  struct file *f;
  struct inode *ip;
  int addr = 0;
  int nchar = 0;

  //Fetch cli argument
  if (argstr(0, path, MAXPATH) < 0)
  {
    printf("sys_symlink: path error\n");
    return -1;
  }
  argint(1, &addr);
  if (!addr)
  {
    printf("sys_symlink: addr error\n");
    return -1;
  }
  argint(2, &nchar);
  if (nchar <= 0)
  {
    printf("sys_symlink: nchar <= 0\n");
    return -1;
  }

  //Increment the number of outstanding syscalls
  begin_op();

  //Create a new empty file, mimic create()
  struct inode *dp;
  char name[DIRSIZ];

  if ((dp = nameiparent(path, name)) == 0)
  {
    printf("sys_symlink: cannot locate parent path\n");
    return -1;
  }

  ilock(dp);

  //does name exist or not
  if ((ip = dirlookup(dp, name, 0)) != 0)
  {
    // name already exists
    iunlockput(dp);
    iput(ip);
    printf("sys_symlink: file already exists\n");
    return -1;
  }

  //allocate inode
  if ((ip = ialloc(dp->dev, T_SLINK)) == 0)
  {
    iunlockput(dp);
    printf("sys_symlink: failed to allocate inode\n");
    return -1;
  }

  ilock(ip);
  ip->nlink = 1;
  iupdate(ip);

  if (dirlink(dp, name, ip->inum) < 0)
    goto fail;

  iunlockput(dp);

  //Allocate a new file and a new file descriptor
  if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0)
  {
    if (f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    printf("sys_symlink: failed to allocate file or file descriptor\n");
    return -1;
  }

  f->type = FD_INODE;
  f->off = 0;
  f->ip = ip;
  f->readable = O_WRONLY;
  f->writable = O_WRONLY || O_RDWR;

  iunlock(ip);
  filewrite(f, addr, /* strlen((char *)addr)*/ nchar);

  //Debugging: Is it OK to readout from inode?
  ilock(ip);
  printf("size of inode: %d\n", ip->size);
  int size = ip->size;
  char test[256] = {0};
  readi(ip, 0, (uint64)test, 0, size);
  printf("test is: %s\n", test);
  iunlock(ip);
  end_op();

  return fd;

fail:
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

//Only creates new, empty file
//If parent directory does not exist, exit with an error message
//If path already exists, exit with an error message
uint64
sys_touch(void)
{
  char path[MAXPATH];
  int fd;
  struct file *f;
  struct inode *ip;

  //Fetch cli argument
  if (argstr(0, path, MAXPATH) < 0)
  {
    printf("sys_touch: path error\n");
    return -1;
  }

  //Increment the number of outstanding syscalls
  begin_op();

  //Create a new empty file, mimic create()
  struct inode *dp;
  char name[DIRSIZ];

  if ((dp = nameiparent(path, name)) == 0)
  {
    printf("sys_touch: cannot locate parent path\n");
    return -1;
  }

  ilock(dp);

  //does name exist or not
  if ((ip = dirlookup(dp, name, 0)) != 0)
  {
    // name already exists
    iunlockput(dp);
    iput(ip);
    printf("sys_touch: file already exists\n");
    return -1;
  }

  //allocate inode
  if ((ip = ialloc(dp->dev, T_FILE)) == 0)
  {
    iunlockput(dp);
    printf("sys_touch: failed to allocate inode\n");
    return -1;
  }

  ilock(ip);
  ip->nlink = 1;
  iupdate(ip);

  if (dirlink(dp, name, ip->inum) < 0)
    goto fail;

  iunlockput(dp);

  //Allocate a new file and a new file descriptor
  if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0)
  {
    if (f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    printf("sys_touch: failed to allocate file or file descriptor\n");
    return -1;
  }

  f->type = FD_INODE;
  f->off = 0;
  f->ip = ip;
  f->readable = O_WRONLY;
  f->writable = O_WRONLY || O_RDWR;

  iunlock(ip);
  end_op();

  return fd;

fail:
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

//Try to find a file in all directories
//Step 1: Loop through all inodes and mark directories
//Step 2: Focus on the directories, and print all dir entry names
//Problem: I only have dinode, how to get inode?
// extern struct superblock sb;
// extern struct bc bcache;

// __attribute__((optimize("O0")))
uint64
sys_find(void)
{
  char path[MAXPATH];
  struct inode *dp;

  //Fetch cli argument
  if (argstr(0, path, MAXPATH) < 0)
  {
    printf("sys_find: path error\n");
    return -1;
  }

  //Find current dev
  if ((dp = myproc()->cwd) == 0)
  {
    printf("sys_find: cannot locate dev\n");
    return -1;
  }

  begin_op();

  //Actually we can go from the root
  //namei() must be inside a transaction (begin_op <-> end_op pair)
  struct inode *ipath = namei("/");
  list(ipath, "/");

  iput(ipath);
  iput(dp);

  end_op();
  return 0;
}

//List all entries under a given inode
//namei() requires full path, so we pass parent path in recursive calls
static void
list(struct inode *ipath, char *parent)
{
  ilock(ipath);
  ASSERT(ipath);
  ASSERT(ipath->inum != 0);
  iunlock(ipath);

  struct dirent dent;

  for (uint off = 0; off < ipath->size; off += sizeof(dent))
  {
    ilock(ipath);
    if (readi(ipath, 0, (uint64)&dent, off, sizeof(dent)) != sizeof(dent))
    {
      printf("list: readi error\n");
      iunlock(ipath);
      break;
    }
    iunlock(ipath);
    if(dent.inum == 0)
      break;

    char fpath[MAXPATH] = {0};
    //If parent is '/' then we don't need to "join"
    if (*parent != '/')
    {
      int i = 0;
      while (1)
      {
        fpath[i] = parent[i];
        i++;
        if (!parent[i])
          break;
      }

      fpath[i++] = '/';

      while (1)
      {
        fpath[i] = dent.name[i-strlen(parent)-1];
        i++;
        if (!dent.name[i-strlen(parent)-1])
          break;
      }
    }
    else
    {
      int i = 0;
      while (1)
      {
        fpath[i] = dent.name[i];
        i++;
        if (!dent.name[i])
          break;
      }
    }
    printf("inum: %d, name: %s, fullpath: %s, ", dent.inum, dent.name, fpath);
    struct inode *ientry = namei(fpath);
    ilock(ientry);
    printf("type: %d\n", ientry->type);

    //Recursively drills down the directory
    if (ientry->type == T_DIR)
    {
      //Skip . and .. to prevent inf recursion
      if (dent.name[0] != '.')
      {
        iunlock(ientry);
        // list(ientry, fullpath);
        list(ientry, fpath);
        //list(ientry, dent.name);
      }
      else
      {
        printf("list: skipping . and ..\n");
        iunlock(ientry);
      }
    }
    //NOTE: Man this took me a few hours to figure out
    //So if I don't unlock(ientry), it actually breaks the NEXT kexec()
    //Because we never iunlock any T_FILE inodes, so in the next kexec()
    //say we want to do kexec("ls"), then ls is actually locked,
    //and ilock(ip) in kexec() is going to hang
    //LINK - kernel/exec.c#list_hang
    else
      iunlock(ientry);
  }
}

uint64
sys_namei(void)
{
  char path[MAXPATH];
  struct inode *dp;

  //Fetch cli argument
  if (argstr(0, path, MAXPATH) < 0)
  {
    printf("sys_find: path error\n");
    return -1;
  }

  //Find current dev
  if ((dp = myproc()->cwd) == 0)
  {
    printf("sys_find: cannot locate dev\n");
    return -1;
  }

  begin_op();
  struct inode *ipath = namei(path);
  printf("inum: %d, type: %d\n", ipath->inum, ipath->type);
  end_op();
  return 0;
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