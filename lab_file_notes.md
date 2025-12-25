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

#### sys_touch()

I want to create something similar to `sys_open()`, but only for creating empty, new files. The userland program is `touch.c` which calls the sys call for heavy lifting. The syscall is named `sys_touch()` for simplicity.

Essentially, this is a smaller version of `sys_open()` which embeds a smaller version of `create()`.

The code is in `sysfile.c` in branch `fs_touch`. This is a pretty easy piece of code as I only need to copy from `create()` and `sys_open()`.

#### sys_find

Now I want to write `find`. 

Jeez the API is so convoluted. Somehow all the conveninet functions like `iget()` are `static`, and there is NO WAY to call these functions from the API. For example, I cannot get an `struct inode` from a random path, such as "/". I don't understand why this is so convoluted. I wonder what is the Windows way to do it.

OK I'm done with most of it. Basically, my `sys_find()` doesn't find names yet, but it recursively goes over all directories and files, so what is left to do, is to match the names. This shouldn't be too hard. I'll describe how I reach this point. There was a lot of learning these 2 days.

The pseudo code looks like this:

```
First we go from the root directory

Then we 
```

### Big File implementation

Hmmm, I'm trying to understand what the specification wants:

> You'll change the xv6 file system code to support a "doubly-indirect" block in each inode, containing 256 addresses of singly-indirect blocks, each of which can contain up to 256 addresses of data blocks. The result will be that a file will be able to consist of up to 65803 blocks, or 256*256+256+11 blocks (11 instead of 12, because we will sacrifice one of the direct block numbers for the double-indirect block). 

From my understanding the scheme looks like this:

addr[0] - addr[10]: pointing to 11 direct blocks
addr[11]: 1 `uint` that points to 256 direct/data blocks
addr[12]: 1 `uint` that points to 256 indirect/pointer blocks, and each of these indirect/pointer blocks points to 256 direct/data blocks

For examples, let's say we have `bn = 2000`, so 2000-11=1989. Then we subtract 256 from it, and 1989-256=1733, which means that this is the No.1733 block (assuming we count from block No.1) in the 256*256 blocks. Now we need to divide it by 256. 1733/256=6, and 1733%256=197. So we can say, when bn=2000, the block is at the 198th layer-2 block (array index 196) pointed to by the 7th layer-1 block (array index 5).

This is a pretty straightforward project. I passed the `bigfile` test but I'm not done yet. I need to modify `itrunc()` too. I'm a bit weak on the consistency these weeks. I need to do better. But at least I should be able to complete all labs in a few weeks.

`itrunc()` is also pretty straightforward to implement. We basically go through the whole FS and `bfree()` any block that is being used -- by checking whether its `blockno` is 0 or not. The only modification I needed to make was for the doubly indirect blocks:

- First read addrs[NDIRECT+1] block, which contains the blockno of 256 layer-1 indirect blocks;
- For each of the layer-1 indirect blocks, if they are "active", read the block, and go through the 256 layer-2 direct blocks;
- Free any "active" layer-2 direct blocks;
- Once all 256 layer-2 direct blocks have been checked/freed, free the respective layer-1 indirect block
- Once all 256 layer-1 indirect blocks have been checked/freed, free addrs[NDIRECT+1]

In above operations, make sure to set the blockno to 0 once called `bfree()` so that future `balloc()` can use the block.

Passed both `bigfile` and `usertests -q`.

```
OK
test lazy_copy: OK
ALL TESTS PASSED
```

### Symbolic link

I never used symbolic links in Linux before, so I decided to try it out in command line. I have an empty file `foo.txt`:

```
markus@t470s:~/dev$ stat foo.txt
  File: foo.txt
  Size: 0         	Blocks: 0          IO Block: 4096   regular empty file
Device: 10302h/66306d	Inode: 10505950    Links: 1
Access: (0664/-rw-rw-r--)  Uid: ( 1000/  markus)   Gid: ( 1000/  markus)
Access: 2025-12-24 07:50:08.104978253 -0500
Modify: 2025-12-19 10:59:03.471543169 -0500
Change: 2025-12-19 10:59:03.471543169 -0500
 Birth: 2025-12-19 10:59:03.471543169 -0500
```

Then I ran `ln --symbolic foo.txt foo.txt.sl`:

```
markus@t470s:~/dev$ stat foo.txt.sl 
  File: foo.txt.sl -> foo.txt
  Size: 7         	Blocks: 0          IO Block: 4096   symbolic link
Device: 10302h/66306d	Inode: 10494627    Links: 1
Access: (0777/lrwxrwxrwx)  Uid: ( 1000/  markus)   Gid: ( 1000/  markus)
Access: 2025-12-24 07:51:05.497835154 -0500
Modify: 2025-12-24 07:51:04.329817680 -0500
Change: 2025-12-24 07:51:04.329817680 -0500
 Birth: 2025-12-24 07:51:04.329817680 -0500
```

So it looks like symbolic link is a different type of file (probably not a file but of the type of symbolic link).

From top of head, I probably need to make sure:

- xv6 knows how to create a symbolic link
- xv6 knows how to exec a symbolic link
- xv6 knows how to rm a symbolic link
- Anything else?

places to touch (just search places referencing T_FILE should give me a good list):
- `stat.h`
- `sys_open()`
- `create()`
- `ls.c`

I'll read and understand `sys_link()` first. It has 3 arguments, `new`, `old` and `name`. `new` is the path new as a link to the same inode as `old`.

The code is straightforward:
- Fetch and check CLI arguments;
- Open a transaction by calling `begin_op()`;
- Fetch the inode of `old` as `ip`, and check if it is a directory inode;
- Increment `ip->nlink`, and update the on disk `dinode`;
- Fetch the inode of the parent `inode` of `new` as `dp`, and dump the filename into `name`;
- If `dp` doesn't have such a directory entry, create one using the pair of `(inum, name)`
- Close the transaction by calling `end_op()`;

For each `inode` operation, the program wraps up the operation with `ilock()` and `iunlock()` (or `iunlockput()`).

#### Use cases

From my understanding, symbolic link is just a different file (with a different type). It does not increment `nlink`.

Think about some usage of symbolic links in Linux:

```sh
touch foo.txt
ln --symbolic foo.txt foo.txt.sl
echo "Hello, world!" > foo.txt
cat foo.txt.sl
```

The above shows "Hello, world!". So we know that somehow, `cat` needs to open the linked file. 

```sh
ln --symbolic foo.txt.sl foo.txt.sl2
cat foo.txt.sl2
```

The above shows the string too. So we know that `cat` recursively find the linked file until it hits a real file. Looking at the code of `cat.c`:

```C
for(i = 1; i < argc; i++){
  if((fd = open(argv[i], O_RDONLY)) < 0){
    fprintf(2, "cat: cannot open %s\n", argv[i]);
    exit(1);
  }
  cat(fd);
  close(fd);
}
```

We see that the code calls `open()`, so we probably need to figure out how to teach `open()` to find the linked file/directory of a symbolic link. Note that there can be an undeterminable number of hops so we need a bit of recursion here.

**sys_open() research:**

Looking at the code in `sys_open()` with `omode` as `O_RDONLY`,

```C
} else {
  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
```

^ `ip` is the `inode` for the symbolic link. I think I can add a field into `inode` to indicate the linked file (this can also be a symbolic link), so that `sys_open()` can get the path of a real file. I don't think we should put that functionality in `namex()` because it is used in a lot of other places, e.g. `sys_link()`. I don't want to break it.

Once this change is done, there is nothing else to modify in `sys_open()` since we already fetched the `inode` for the linked file.

The proposed change is as following. I want to make sure that it is protected by locks properly.

```C
struct *inode tempip = ip;
if (tempip->type == T_SLINK)
{
  while (tempip->type == T_SLINK)
  {
    ilock(tempip);
    if ((ip = namei(tempip->targetpath)) == 0)
    {
      iunlockput(tempip);
      end_op();
      return -1;
    }
    iunlock(tempip);
  }
  //Separate iunlock and iput as still need tempip in next loop
  iput(tempip);
}
```

**create() research**

When we create a new symbolic link, we definitely need to create a new FS item. For hard links, `sys_link()` doesn't create any new item (hard link does not even have its own type in `stat.h`), but gets away by  creating a directory entry by calling `dirlink()`.

Actually, maybe I don't need to change the code in `create()`. Since `create()` doesn't know the target path, `create()` should just create the `inode` and then `sys_slink()` can change the value of `inode.target` as it does know the target path.

**chdir() research**

Since symbolic links can target directories, this syscall needs to figure out the real path. I kinda think I should wrap up the ^ proposed change and move to a function? Or maybe not, because of all those locks...


#### First trail

OK first trail failed very quickly. I tried to add a `char[]` in `inode` and `dinode` to store the target path of a symbolic link, but `mkfs.c` actually checks whether `(BSIZE % sizeof(struct dinode)) == 0`, which means I can't alter these two `struct` easily. I mean, I could still alter the number of bytes and perhaps make it work, but I need to check if I can store the information in some other places.

Actually, I'm thinking, it's a lot easier to store the inum of the target path. But this still needs a piece of new data to be stored somewhere, and not in `inode` or `dinode`...hmmm, that's inconvenient.

OK I think I got it, re-reading the OSTEP book reveals that a symbolic link in Linux (xv6 can follow the same principle) simply holds the pathname in the file itself. So that means I need to `writei()` and `readi()`, I think. Gotta take a break and figure it out tomorrow.