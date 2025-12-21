### File System Summary

Layers          ->source file(s)      ->key data structure                              ->some functions:

File descriptor -> `file.h`, `file.c` -> `struct file`, `ftable` (under `struct proc`)  ->`filealloc()`, `filedup()`, `fileclose()`

Pathname        -> `fs.c`             ->                                                -> `namex()`

Directory       -> `fs.c`, `fs.h`     -> `struct dirent`                                -> `dirlookup()`, `dirlink()`

Inode           -> `fs.c`, `fs.h`, `file.c`, `file.h` -> `struct inode`, `struct dinode`, `itable`  -> `ialloc()`, `iupdate()`, `iget()`, `idup()`, `ilock()`, `iunlock()`, `iput()`, `iunlockput()`, `ireclaim()`, `bmap()`, `itrunc()`, `readi()`, `writei()`

Logging         -> `log.c`            -> `struct logheader`, `struct log`               -> `begin_op()`, `end_op()`, `write_log()`, `commit()`, `log_write()`, `recover_from_log()`

Buffer cache    -> `bio.c`, `buf.h`   -> `struct buf`, `struct bcache`                  -> `binit()`, `bget()`, `bread()`, `bwrite()`, `brelse()`, `bpin()`, `bunpin()`

Disk            -> `virtio_disk.c`

### inodes

I'm still trying to fully understand inodes as they seem to be the core of the fs. You want to read from the disk? Go through inodes. You want to create a new file? Go through inodes. They are the middlemen between buf and file/dir layers. 

Note that each file/device/directory has an (d)inode, which stores the metadata.

#### *Where are inodes stored?*

They are stored in buffers. Recall that (d)inodes are also stored on disk so they occupy buffers in specific places:
```
boot | superblock | log | inodes | bitmap | data
0    | 1          | 2   | 4      | 7      | 9 
```
The block number of the first dinode is stored in `sb.inodestart` (`sb` means superblock). Each block has `BSIZE`, 1024 bytes, and each dinode has 64 bytes, so each dinode block contains 1024/64 = 16 dinodes. To go through each dinode, follow the code in `ialloc()`:

```C
for(inum = 1; inum < sb.ninodes; inum++){
    //NOTE - Recall that (d)inodes are also stored on disk so they occupy buffers in specific places
    //boot | superblock | log | inodes | bitmap | data
    //0    | 1          | 2   | 4      | 7      | 9 
    //IBLOCK = inum / IPB + sb.inodestart = inum / (BSIZE / sizeof(struct dinode)) + sb.inodestart
    //BSIZE=1024, sizeof(struct dinode)=64, so IBLOCK = inum/16 + sb.inodestart
    //sb.inodestart = block number of the first inodeblock
    //So in the first 15 iteration, IBLOCK() = sb.inodestart, in the next 16 iterations, it's 1+sb.inodestart
    //TODO: I have no idea why inum starts from 1, not 0...
    bp = bread(dev, IBLOCK(inum, sb));
    //One inode block has IPB=16 inodes -> each 16 loops has the same bp -> use inum%IPB for individual 64-byte inode
    dip = (struct dinode*)bp->data + inum%IPB;
    //...
}
```
Some comments on IBLOCK calculation:
```
IBLOCK = inum / IPB + sb.inodestart = inum / (BSIZE / sizeof(struct dinode)) + sb.inodestart
BSIZE=1024, sizeof(struct dinode)=64, so IBLOCK = inum/16 + sb.inodestart
```

#### *Directory inodes*

Directory inodes have the same structures comparing to file/device inodes. But the buffers (blocks), of which its `addr` field contains the block numbers, store `struct dirent` (directory entries) in `buffers.data` field.

To loop through `struct dirent` for an inode, reference the code in `dirlink()`:

```C
for(off = 0; off < dp->size; off += sizeof(de)){
  if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("dirlink read");
  if(de.inum == 0)
    break;
}
```
`readi()` doesn't have visilibity what type of data we want to read. It simply use `off` (offset) to calculate the block number, then go to that block and reads `sizeof(de)` bytes of `block.data` into an address.

```C
struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  struct buf *prev; // LRU cache list
  struct buf *next;
  uchar data[BSIZE];
};
```
Each buffer has BSIZE (1024) bytes in its `data`, and `sizeof(de)` is 16, so each "directory" (block doesn't really know what type of data it stores) block has 64 `struct dirent` stored in its `data` field.

#### *Where is filename stored and how to connect filename with inode?*

Each file is always under some directory, so each is a directory entry. The idea is to go through each directory inode -> get block number -> go through buffer.data for each directory entry `struct dirent` -> find the one that matches the filename -> fetch the `inum` field which is its inode number.

Once we obtain the inode number of the filename, we can go anywhere. There is a global `itable` that stores all inodes and we can use `iget()` to fetch a pointer to the `struct inode`.

Please note that inode number (`inum`) is only unique FOR THE SAME DEVICE (`dev`), so `iget()` checks both `dev` and `inum`. It also creates the inode if it is not in `itable`.

#### *What does cd do*

If we `cd testdir1` and set a breakpoint in `sys_chdir()`:

- It fetches the inode for the path using `namei()`, which calls `namex()`.
- Then it simply set `p->cwd` to `ip`. Both are pointers to `inode`.

### logs

#### *log is a global struct log object*


### Tracing fs syscalls

#### Run `echo "Hello" > hello.txt` in xv6 and `bp create()` in gdb

Frame:
`create (path=path@entry=0x3fffff9f10 "hello.txt", type=type@entry=2, major=major@entry=0, minor=minor@entry=0)`

I'm curious how `nameiparent()` works, because `path` now is just a filename, without the implicit `./` parent directory.

Frame:
`namex (path=path@entry=0x3fffff9f10 "hello.txt", nameiparent=nameiparent@entry=1, name=name@entry=0x3fffff9eb0 "\340\236\377\377?")`

Turns out, if `path` doesn't contain `'/'` as the first char, it finds the current working directory by checking `myproc()->cwd`.

```C
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  //-> Goes into this branch
  else
    ip = idup(myproc()->cwd);

  while((path = skipelem(path, name)) != 0){
    //Now path is "", and name is "hello.txt"
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      //-> Goes into this branch, ip is the inode of cwd
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    // Rest of the code
  }
}
```

Then `create()` tries to check whether `name` already exists by using `dirlookup()`, which it doesn't. So it calls `ialloc()` to allocate a new inode. `ialloc()` goes through the blocks that contain inode data, and find one that is free: //LINK - kernel/fs.c#free_inode

Then `create()` sets up data in that inode, and signal FS that we want to write it to disk by calling `iupdate()`. Note that the FS eventually calls `commit()` to commit all transactions. `iupdate()` is simply a signal, not a commit (you won't see it on disk until the transaction has been COMMITTED).

- I think the key to understand the logic of `iupdate()` is to recognize that the buffer layer is the closet to the metal, interacting with virtio disk driver to read/write from/into disk. 
- So every change that we want to save to the disk involves some buffer block.
- And everything we read from disk involves going through the blocks, so you will see `bread()` a lot.
- `dinode` is the on-disk `inode`, which contains all the metadata of a file. So `iupdate()` grabs some buffer first, and then use that buffer to store a `dinode`. 
- It copies the data from `inode` to `dinode`, and calls `log_write()` to notify the log system (transaction) that I want to add this block into the next batch of commit.

```C
void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  //NOTE - Since we already cast bp->data to (struct dinode*),
  //Pointer arithmetic means (struct dinode*)bp->data + 1 actually
  //points to the byte address (bp->data + sizeof(struct dinode)),
  //which is bp->data + 64
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  //NOTE - sizeof(ip->addrs) is 13 * 4 = 52 bytes, not 4 bytes!
  //Arrays don't decay into pointers in sizeof(), as sizeof() is an OPERATOR,
  //and arrays only decay into pointers when passed to FUNCTIONs.
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  log_write(bp);
  //TODO - Why do we need to call brelse()?
  //At first I thought because we already signalled the FS that we want to write bp
  //LINK - kernel/log.c#signal_log
  //But then I realized that brelse() reduces the refcnt, but where was refcnt incremented?
  //Eventually I found out that bread() calls bget() which increments the refcnt
  //I'm still not 100% sure so need to debug again (bp iupdate() in GDB)
  brelse(bp);
}
```

Then `create()` calls `dirlink()` to link `name` to `dp`. That is, it writes the new `struct dirent` into `dp`. Recall that if `dp` is a directory inode, its `data` field contains a list of `struct dirent`.

`dirlink()` find an empty `struct dirent` (read every `struct dirent` into `de` with `readi()`, and then find one with `de.inum == 0`), fill in `inum` and `name`, then calls `writei()` to write `de` back into `dp`.

This is pretty much it. HOWEVER, I still don't know when FS commits the change -- when does it actually writes `hello4.txt` into the disk? `commit()` is referenced by `end_op()`, which itself is called by many FS functions. We keep tracing.

Once `create()` is done, we are back to `sys_open()` because it is the function that calls `create()`. It eventually calls `filealloc()` to allocate a new file and a new file descriptor.

So a bit of summary of all data structures touched when creating a new file.

- Need to find an empty `struct inode`
- Need to find an empty `struct dinode` to write to disk
- Need to find a `struct file *f` with `f->ref == 0`
- Need to go into `proc->ofile[]` to find one that is 0, and set it to `f` (returned from last step), this is to allocate a file descriptor

There are other data structures such as `struct dirent`, `struct itable`, `struct ftable`, etc. that I don't quite remember. I don't think I command the mindset of the FS, so maybe I'll program a few FS syscalls without using the existing code.

### Creating fs syscalls

I want to create something similar to `sys_open()`, but only for creating empty, new files. The userland program is `touch.c` which calls the sys call for heavy lifting. The syscall is named `sys_touch()` for simplicity.

Essentially, this is a smaller version of `sys_open()` which embeds a smaller version of `create()`.

The code is in `sysfile.c` in branch `fs_touch`. This is a pretty easy piece of code as I only need to copy from `create()` and `sys_open()`.

Now I want to write `find`. 

Jeez the API is so convoluted. Somehow all the conveninet functions like `iget()` are `static`, and there is NO WAY to call these functions from the API. For example, I cannot get an `struct inode` from a random path, such as "/". I don't understand why this is so convoluted. I wonder what is the Windows way to do it.