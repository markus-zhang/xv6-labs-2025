#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#define stat xv6_stat  // avoid clash with host struct stat
#include "kernel/types.h"
#include "kernel/fs.h"
#include "kernel/stat.h"
#include "kernel/param.h"

#ifndef static_assert
#define static_assert(a, b) do { switch (0) case 0: case (a): ; } while (0)
#endif

#define NINODES 200

// Disk layout:
// [ boot block | sb block | log | inode blocks | free bit map | data blocks ]

int nbitmap = FSSIZE/BPB + 1;
int ninodeblocks = NINODES / IPB + 1;
int nlog = LOGBLOCKS+1;   // Header followed by LOGBLOCKS data blocks.
int nmeta;    // Number of meta blocks (boot, sb, nlog, inode, bitmap)
int nblocks;  // Number of data blocks

int fsfd;
struct superblock sb;
char zeroes[BSIZE];
uint freeinode = 1;
uint freeblock;


void balloc(int);
void wsect(uint, void*);
void winode(uint, struct dinode*);
void rinode(uint inum, struct dinode *ip);
void rsect(uint sec, void *buf);
uint ialloc(ushort type);
void iappend(uint inum, void *p, int n);
void die(const char *);

// convert to riscv byte order
ushort
xshort(ushort x)
{
  ushort y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  return y;
}

uint
xint(uint x)
{
  uint y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  a[2] = x >> 16;
  a[3] = x >> 24;
  return y;
}

int
main(int argc, char *argv[])
{
  int i, cc, fd;
  uint rootino, inum, off;
  struct dirent de;
  char buf[BSIZE];
  struct dinode din;


  static_assert(sizeof(int) == 4, "Integers must be 4 bytes!");

  if(argc < 2){
    fprintf(stderr, "Usage: mkfs fs.img files...\n");
    exit(1);
  }

  assert((BSIZE % sizeof(struct dinode)) == 0);
  assert((BSIZE % sizeof(struct dirent)) == 0);

  //fsfd is the file descriptor of fs.img in practice.
  fsfd = open(argv[1], O_RDWR|O_CREAT|O_TRUNC, 0666);
  if(fsfd < 0)
    die(argv[1]);

  // 1 fs block = 1 disk sector
  nmeta = 2 + nlog + ninodeblocks + nbitmap;
  nblocks = FSSIZE - nmeta;

  //Super block is block 1 (block 0 is boot)
  sb.magic = FSMAGIC;
  sb.size = xint(FSSIZE);
  sb.nblocks = xint(nblocks);
  sb.ninodes = xint(NINODES);
  sb.nlog = xint(nlog);
  sb.logstart = xint(2);
  sb.inodestart = xint(2+nlog);
  sb.bmapstart = xint(2+nlog+ninodeblocks);

  printf("nmeta %d (boot, super, log blocks %u, inode blocks %u, bitmap blocks %u) blocks %d total %d\n",
         nmeta, nlog, ninodeblocks, nbitmap, nblocks, FSSIZE);

  //This is probably the index of the first data block
  freeblock = nmeta;     // the first free block that we can allocate

  //Write zeros to every sector/block. Each sector/block has BSIZE (0x1000) bytes.
  //XV6 FS has a total of 2,000 sectors/blocks.
  for(i = 0; i < FSSIZE; i++)
    wsect(i, zeroes);

  //Copy the content of superblock sb into the buffer, 
  //then write the buffer into sector/block 1
  //TODO: Can we write sb into sector/block 1 instead of using a buffer?
  //Something like: wsect(1, (void *)(&buf))
  memset(buf, 0, sizeof(buf));
  memmove(buf, &sb, sizeof(sb));
  wsect(1, buf);

  //Allocate an inode and return the inum index as rootino.
  //This is the inode of the root directory. It should have inum 1.
  rootino = ialloc(T_DIR);
  assert(rootino == ROOTINO);

  //This bzero() is NOT the one in fs.c as that one is static, but the one in host OS.
  //https://man7.org/linux/man-pages/man3/bzero.3.html
  //mkfs.c uses a "template" de to make . and .. for the root directory.
  //NOTE: I have inspected the fs.img file. The dinode of root directory is at address 0x8440.
  //This is because the inum of the root directory inode is 1, not 0, and each dinode has 64 bytes of size. 0x8400 + 0x40 = 0x8450, where 0x8400 is calculated by: sb.inodestart = xint(2+nlog), which is block 33, so 33 * 1,024 = 0x8400
  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, ".");
  iappend(rootino, &de, sizeof(de));

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, "..");
  iappend(rootino, &de, sizeof(de));

  //For each file in the argument list.
  for(i = 2; i < argc; i++){
    // get rid of "user/"
    char *shortname;
    //if filename contains user/ -> check UEXTRA in Makefile
    //There are two files with user/ prefix
    if(strncmp(argv[i], "user/", 5) == 0)
      shortname = argv[i] + 5;
    else
      shortname = argv[i];
    
    //This is another host library (non-xv6) function.
    //returns 0 if not found. This means filenames such as abcd/ or user/blah/mau don't pass.
    //NOTE: I think this is really limiting. What if I have a file /user/test/blah.c?
    assert(index(shortname, '/') == 0);

    if((fd = open(argv[i], 0)) < 0)
      die(argv[i]);

    // Skip leading _ in name when writing to file system.
    // The binaries are named _rm, _cat, etc. to keep the
    // build operating system from trying to execute them
    // in place of system binaries like rm and cat.
    if(shortname[0] == '_')
      shortname += 1;

    assert(strlen(shortname) <= DIRSIZ);
    
    //OK looks like mkfs.c defaults to just one directory -- the root one.
    //Everything else is a file in default.
    //This is probably why it stripped off the user/ in the beginning of the loop.
    inum = ialloc(T_FILE);

    //Recall that inode (in this case the inode of the root directory)
    //contains directory entries in its block(s).
    bzero(&de, sizeof(de));
    de.inum = xshort(inum);
    strncpy(de.name, shortname, DIRSIZ);
    iappend(rootino, &de, sizeof(de));

    //File may be larger than 0x400 bytes.
    //It takes more than one buffer to contain the file.
    while((cc = read(fd, buf, sizeof(buf))) > 0)
      iappend(inum, buf, cc);

    close(fd);

    //Summarize: this loop first fixes the filename, and copies it into de.name.
    //Then it appends the de (directory entry) onto the inode of the root directory.
    //Finally it appends its own content onto its own inode.
    //TODO: Figure out how these blocks are written into the FS. I don't see wsect().
    //I think the key is in iappend().
  }

  // fix size of root inode dir
  //Store the address of the rootino inode into &din. Changing din changes that inode.
  rinode(rootino, &din);
  off = xint(din.size);
  //TODO: Figure out why mkfs.c needs to set din.size to ceiling(din.size).
  off = ((off/BSIZE) + 1) * BSIZE;
  din.size = xint(off);
  winode(rootino, &din);

  balloc(freeblock);

  exit(0);
}

void
wsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE)
    die("lseek");
  if(write(fsfd, buf, BSIZE) != BSIZE)
    die("write");
}

void
winode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;

  bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  dip = ((struct dinode*)buf) + (inum % IPB);
  *dip = *ip;
  wsect(bn, buf);
}

void
rinode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;

  //Get block number from inum. Expands to:
  //((inum) / (1024 / sizeof(struct dinode)) + sb.inodestart)
  //sb.inodestart is the block number of the first inode block.
  bn = IBLOCK(inum, sb);
  //Read one sector/block into buf.
  rsect(bn, buf);
  //Assume each directory entry is 16 bytes, this means IPB=64.
  //Let's say inum = 70, so we get buf + 6, sort of wrap around.
  //NOTE: This might be conter-intuitive, but the fact is,
  //we already got the block number of the inode, so the "offset"
  //must be of [0, 64]. There is no way it belongs to the next block.
  dip = ((struct dinode*)buf) + (inum % IPB);
  *ip = *dip;
}

void
rsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE)
    die("lseek");
  if(read(fsfd, buf, BSIZE) != BSIZE)
    die("read");
}

uint
ialloc(ushort type)
{
  uint inum = freeinode++;
  struct dinode din;

  bzero(&din, sizeof(din));
  din.type = xshort(type);
  din.nlink = xshort(1);
  din.size = xint(0);
  winode(inum, &din);
  return inum;
}

void
balloc(int used)
{
  uchar buf[BSIZE];
  int i;

  printf("balloc: first %d blocks have been allocated\n", used);
  assert(used < BPB);
  bzero(buf, BSIZE);
  for(i = 0; i < used; i++){
    buf[i/8] = buf[i/8] | (0x1 << (i%8));
  }
  printf("balloc: write bitmap block at sector %d\n", sb.bmapstart);
  wsect(sb.bmapstart, buf);
}

#define min(a, b) ((a) < (b) ? (a) : (b))

void
iappend(uint inum, void *xp, int n)
{
  char *p = (char*)xp;
  uint fbn, off, n1;
  struct dinode din;
  char buf[BSIZE];
  uint indirect[NINDIRECT];
  uint x;

  rinode(inum, &din);
  off = xint(din.size);
  // printf("append inum %d at off %d sz %d\n", inum, off, n);
  while(n > 0){
    fbn = off / BSIZE;
    assert(fbn < MAXFILE);
    if(fbn < NDIRECT){
      if(xint(din.addrs[fbn]) == 0){
        din.addrs[fbn] = xint(freeblock++);
      }
      x = xint(din.addrs[fbn]);
    } else {
      if(xint(din.addrs[NDIRECT]) == 0){
        din.addrs[NDIRECT] = xint(freeblock++);
      }
      rsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      if(indirect[fbn - NDIRECT] == 0){
        indirect[fbn - NDIRECT] = xint(freeblock++);
        wsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      }
      x = xint(indirect[fbn-NDIRECT]);
    }
    n1 = min(n, (fbn + 1) * BSIZE - off);
    rsect(x, buf);
    bcopy(p, buf + off - (fbn * BSIZE), n1);
    wsect(x, buf);
    n -= n1;
    off += n1;
    p += n1;
  }
  din.size = xint(off);
  winode(inum, &din);
}

void
die(const char *s)
{
  perror(s);
  exit(1);
}
