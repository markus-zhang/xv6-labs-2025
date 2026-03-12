**Note that all these researches are based on the mmap branch.**

## Mental Model of this research:

- What on-disk structures store the file system's data and metadata?

- What happens when a process opens a file?

- Which on-disk structures are accessed during a read or write?

## Data Structure

On disk, the bytes of the file system are represented by "blocks". In xv6, each block is defined as 1,024 bytes, in `BSIZE`.

The first block of the file system is the **superblock**, defined in `struct superblock`:

```C
struct superblock {
  uint magic;        // Must be FSMAGIC
  uint size;         // Size of file system image (blocks)
  uint nblocks;      // Number of data blocks
  uint ninodes;      // Number of inodes.
  uint nlog;         // Number of log blocks
  uint logstart;     // Block number of first log block
  uint inodestart;   // Block number of first inode block
  uint bmapstart;    // Block number of first free map block
};
```

## Access Methods

### Tracing a cat

I ran `cat README` in xv6 and record the whole tracing process below:

**STEP 1:** `main()` calls `open()`, which is a system call stub, that calls the real syscall `sys_open()` routine. Technically, `open()` is defined in `usys.S`, which just loads `SYS_open`, the integer `15` (defined in `syscall.h`) into `a7` register. Then it executes `ecall`, which calls the `syscall()` routine eventually. `syscall()` then calls `sys_open()`;

**STEP 2:** We are now in the `sys_open()` routine. Since we are not creating a new file, but reading an existing file, the routine goes into the `if((ip = namei(path)) == 0)` branch -- right now, the Operating System only knows the path of the file we would like to `cat`, and it needs to find it first;

**STEP 3:** `namei()` calls `namex()`. Note that the string `path` is `README`, so the routine knows that we are looksing for something in the CWD, thus it calls `idup()` to return the inode of CWD. It happens that `myproc()->cwd` is an inode, so it only needs to increase the reference count, and return it. I'll show the content of `ip` (a `struct inode`) below. Please note that `addrs` shows that the file it points to (which is the CWD) only takes one block -- block 47. It is also of type 1, which is `T_DIR`, a directory. Last but not least, note that the ref is 4.

```
(gdb) p *ip
$5 = {
  dev = 1,
  inum = 1,
  ref = 4,
  lock = {
    locked = 0,
    lk = {
      locked = 0,
      name = 0x800086d8 "sleep lock",
      cpu = 0x0 <cat>
    },
    name = 0x800085b8 "inode",
    pid = 0
  },
  valid = 1,
  type = 1,
  major = 0,
  minor = 0,
  nlink = 1,
  size = 1024,
  addrs = {47, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
}

```

**STEP 4:** Next, `namex()` calls `skipelem()` repeatly to find the last "segment" of the path (read `skipelem()` for details) -- in our case is `README` because we only have one segment. It leaves the string `path` to be empty and essentially moves `README` into `name`.

**STEP 5:** Since `ip->type` is indeed `T_DIR`, and `nameiparent` is 0, the routine goes into `if((next = dirlookup(ip, name, 0)) == 0)` branch. It already got the `name` of the file we want to open, in this case `README`, and proceeds to look up its directory entry, and use that information to find the `struct inode*` of the file `README`. I'd like to remind the reader that the data structure of a directory entry inode looks like this:

```C
// The name field may have DIRSIZ characters and not end in a NUL
// character.
struct dirent {
  ushort inum;
  char name[DIRSIZ] __attribute__((nonstring));
};
```

The most important thing here is the `inum`, which is the index of the `struct inode` in the inode table `itable`.

**STEP 6:** `dirlookup()` needs to loop through each `struct dirent` "represented" by the `struct inode` of CWD, and find the one that has its `name` matches `README`. If the reader reads the code carefully, he/she will understand that, essentially, the routine uses `off`, the offset, to probe into `dp->addr` (in `readi()`) -- and each read dumps a `struct dirent` into `de`. 

Please allow me to repeat because it is really important to understand the low level details of this routine.
- The routine needs to find a `struct dirent` that has its `name` member matches `README`;
- To do so, it needs to load the `struct dirent` into the variable `dp`, becuase that data structure is on disk;
- For a directory inode, the blocks it occupies is simply a sequence of `struct dirent` objects. The reader can read `dirlink()` to find how this is implemented -- it fetches an empty `struct dirent`, fills in `name`, and then write it into the block;
- Once it loads the address of `struct dirent` into `dp`, it then compares `dp->name` with `README`. It loops through all `struct dirent`, until it finds a match;
- It then uses the `inum` to get the `struct inode*` of the file `README`;

This `struct inode*` is returned as `next`. And copied into `ip`, which is returned from `namex()`.

**STEP 7:** Now let's go back to `sys_open()`. Recall that `ip` is no longer the `struct inode` of CWD, but of the file `README`. Now the routine needs to allocate a `file*`, `f`, and a file descriptor, `fd`. I'd say a `struct file` is essentially a wrapper of the `struct inode` with a bunch of other information about the file:

```C
struct file {
  enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE } type;
  int ref; // reference count
  char readable;
  char writable;
  struct pipe *pipe; // FD_PIPE
  struct inode *ip;  // FD_INODE and FD_DEVICE
  uint off;          // FD_INODE
  short major;       // FD_DEVICE
};
```

However, `sys_open()` does not return the `struct file*` directly, but instead returns the file descriptor. This might be a bit confusing because the file descriptor is simply a `static int`. However, the file descriptor is the index into the `struct file*` array `ofile` in the process. Here is one important feature of this arrangement: the process needs to know which files it opens, so it keeps an array of `struct file*`.

**STEP 8:** So all of the above is just to get a file descriptor. Once `cat` has the file descriptor, it can use it to `read()` the content of the file.

```C
void
cat(int fd)
{
  int n;

  while((n = read(fd, buf, sizeof(buf))) > 0) {
    if (write(1, buf, n) != n) {
      fprintf(2, "cat: write error\n");
      exit(1);
    }
  }
  if(n < 0){
    fprintf(2, "cat: read error\n");
    exit(1);
  }
}
```

`read()` is just a wrapper for `fileread()`, which reads the content of the file into a user virtual address. In this case it is `buf`.

**TODO**: Finish the `read()` part. But essentially I believe it just uses `readi()` to read `README` into `buf`.

`write()` is just a wrapper for `filewrite()`, which writes into the console device `f`. Recall that everything in xv6 is a file, and the console device is one, too.

```
(gdb) p *f
$13 = {
  type = FD_DEVICE,
  ref = 9,
  readable = 1 '\001',
  writable = 1 '\001',
  pipe = 0x0 <cat>,
  ip = 0x80021358 <itable+160>,
  off = 0,
  major = 1
}

```

Since `f->type` is `FD_DEVICE`, the routine calls the write routine for that specific device --in our case it is `consolewrite()`. I'm not familiar with the device driver, but looks like it breaks down the file into chunks of 32 bytes, and call `uartwrite()` to write each chunk, until it exhausts `README`. `uartwrite()` simply writes each byte of a chunk into a device register and calls it a day.


I have pondered a question -- why does `cat` have to go between user and kernel space thrice just to show some contents onto the screen? It `open()` the file, and then `read()` its content, and finally `write()` into `stdout`. It also wastes a bunch of cycles creating a `struct file*` and its repsective file descriptor, returning it, and then using it to probe into `ofile` to grab the same `struct file*` just for `fileread()` to read it. It seems a huge waste of CPU cycles as we already have the `struct file*` during `open()`. Can we simply setup a system call that opens, reads and prints a file into `stdout` all in kernel space? Does this even make sense? 

**SUMMARY**

We have a bunch of concepts about a file: block, inode, file and fd.

- Content of file is saved in blocks. The index of these blocks are saved in its `inode`.

- When user reads a file, he/she only knows the name of the file. The internals of the FS is not exposed to the user. For example, he cannot grab a block, or a `struct file*`. User land routines only has access to `open()` and `read()`.

## Advanced projects

- Increment `BSIZE` to 4,096 bytes.

- Add `time`, the epoch of the last time the file is accessed, into `struct inode` and `struct dinode`.

- How can I version the file? Everytime the user changes the file, the FS creates a log file containing the new blocks, and "apply" on the disk when it is flushed. Maybe I can use this log structure to figure out how to undo the changes.

- In `struct inode` and `struct dinode`, `addrs[]` contains the address (integer offset) of all of the blocks that the file takes. Even given a 4KBytes block, the maximum number of 4-byte integers is around 1,000, considering we need some space for other members of a `struct inode`. The maximum size of a file, henceforth, is around 1,000 * 4KBytes < 4MBytes. If `addrs` is only a `uint` that points to the first block, and each block contains a pointer pointing to the next block, wouldn't this design significantly reduces the size of the `struct inode` and the `struct dinode`?

Something like this:

inode.addr -> block 0 buf.file_next -> block 1 buf.file_next -> ...

This is definitely going to be slow for random access, though. Consider that we want to read the last block of a file, but under the extreme situation that each block of the file is on disk, not cached in memory -- the FS needs to load block 0 from disk, find the pointer, and then load block 1 from disk, find the pointer, and so on, until at last it can read the last block.