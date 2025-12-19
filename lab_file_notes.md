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

OK directories are easy because `struct dirent` contains a `name` field which is the name of the directory. But what about filenames? Well, actually, each file is always under some directory, so each file is a directory entry. So the idea is to go through each directory inode -> get block number -> go through buffer.data for each directory entry `struct dirent` -> find the one that matches the filename -> fetch the `inum` field which is its inode number.

Once we obtain the inode number of the filename, we can go anywhere. There is a global `itable` that stores all inodes and we can use `iget()` to fetch a pointer to the `struct inode`.

Please note that inode number (`inum`) is only unique FOR THE SAME DEVICE (`dev`), so `iget()` checks both `dev` and `inum`. It also creates the inode if it is not in `itable`.

### logs

#### *log is a global struct log object*

