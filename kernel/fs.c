// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb; 

// Read the super block.
static void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Init fs
void
fsinit(int dev) {
  readsb(dev, &sb);
  if(sb.magic != FSMAGIC)
    panic("invalid file system");
  initlog(dev, &sb);
  ireclaim(dev);
}

// Zero a block.
static void
bzero(int dev, int bno)
{
  struct buf *bp;

  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  //ANCHOR[id=brelse_example]
  brelse(bp);
}

// Blocks.

// Allocate a zeroed disk block.
// returns 0 if out of disk space.
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;

  bp = 0;
  for(b = 0; b < sb.size; b += BPB){
    bp = bread(dev, BBLOCK(b, sb));
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++){
      m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0){  // Is block free?
        bp->data[bi/8] |= m;  // Mark block in use.
        log_write(bp);
        brelse(bp);
        bzero(dev, b + bi);
        return b + bi;
      }
    }
    brelse(bp);
  }
  printf("balloc: out of blocks\n");
  return 0;
}

// Free a disk block.
static void
bfree(int dev, uint b)
{
  struct buf *bp;
  int bi, m;

  bp = bread(dev, BBLOCK(b, sb));
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  bp->data[bi/8] &= ~m;
  log_write(bp);
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at block
// sb.inodestart. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a table of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The in-memory
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in table: an entry in the inode table
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a table entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   table entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays in the table and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The itable.lock spin-lock protects the allocation of itable
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold itable.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct {
  /*NOTE - lock protects 
    1. The invariant that an inode is present in itable at most once
    2. The invariant that an inode's ref field counts the # of in-memory pointers to the inode
      //LINK - kernel/fs.c#itable_lock_ex1
  */
  struct spinlock lock;
  struct inode inode[NINODE];
} itable;

void
iinit()
{
  int i = 0;
  
  initlock(&itable.lock, "itable");
  for(i = 0; i < NINODE; i++) {
    initsleeplock(&itable.inode[i].lock, "inode");
    //Symbolic Link Lab
    //memset(itable.inode[i].namelnk, 0, DIRSIZ);
  }
}

static struct inode* iget(uint dev, uint inum);

// Allocate an inode on device dev.
// Mark it as allocated by  giving it type type.
// Returns an unlocked but allocated and referenced inode,
// or NULL if there is no free inode.
struct inode*
ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;

  /*NOTE - Loops over inode structure ON DISK,
    Pick a free dinode, mark it allocated ON DISK,
    then call iget() to add it into itable
  */
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
    //ANCHOR[id=free_inode]
    if(dip->type == 0){  // a free inode
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      log_write(bp);   // mark it allocated on the disk
      brelse(bp);
      return iget(dev, inum);
    }
    brelse(bp);
  }
  printf("ialloc: no inodes\n");
  return 0;
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk.
// Caller must hold ip->lock.
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

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
static struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&itable.lock);

  // Is the inode already in the table?
  empty = 0;
  for(ip = &itable.inode[0]; ip < &itable.inode[NINODE]; ip++){
    //NOTE - if inode already in itable, increment ref and return it
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      //ANCHOR[id=itable_lock_ex1]
      ip->ref++;
      release(&itable.lock);
      //NOTE - ip is returned with NON-EXCLUSIVE access, caller needs to call ilock()
      return ip;
    }
    //NOTE - records the position of the FIRST empty slot
    if(empty == 0 && ip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode entry.
  //NOTE - If inode not found in table, and there is no empty slot, panic
  if(empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  release(&itable.lock);

  //NOTE - ip is returned with NON-EXCLUSIVE access, caller needs to call ilock()
  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
idup(struct inode *ip)
{
  acquire(&itable.lock);
  ip->ref++;
  release(&itable.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
//NOTE - Separation of iget() and ilock() solves some deadlock (e.g. during directory lookup)
// Multiple processes can hold a C pointer to an inode returned by iget(),
// but only one process can lock it at a time.
void
ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);

  //NOTE - if inode has not been read into memory, read from disk
  if(ip->valid == 0){
    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode*)bp->data + ip->inum%IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode table entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
void
iput(struct inode *ip)
{
  acquire(&itable.lock);

  if(ip->ref == 1 && ip->valid && ip->nlink == 0){
    // inode has no links and no other references: truncate and free.

    // ip->ref == 1 means no other process can have ip locked,
    // so this acquiresleep() won't block (or deadlock).
    acquiresleep(&ip->lock);

    release(&itable.lock);

    itrunc(ip);
    ip->type = 0;
    iupdate(ip);
    ip->valid = 0;

    releasesleep(&ip->lock);

    acquire(&itable.lock);
  }

  ip->ref--;
  release(&itable.lock);
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

void
ireclaim(int dev)
{
  for (int inum = 1; inum < sb.ninodes; inum++) {
    struct inode *ip = 0;
    struct buf *bp = bread(dev, IBLOCK(inum, sb));
    struct dinode *dip = (struct dinode *)bp->data + inum % IPB;
    if (dip->type != 0 && dip->nlink == 0) {  // is an orphaned inode
      printf("ireclaim: orphaned inode %d\n", inum);
      ip = iget(dev, inum);
    }
    brelse(bp);
    if (ip) {
      begin_op();
      ilock(ip);
      iunlock(ip);
      iput(ip);
      end_op();
    }
  }
}

// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
// returns 0 if out of disk space.
//NOTE - bmap() returns the block number
// static uint
// bmap(struct inode *ip, uint bn)
// {
//   uint addr, *a;
//   struct buf *bp;

//   if(bn < NDIRECT){
//     if((addr = ip->addrs[bn]) == 0){
//       addr = balloc(ip->dev);
//       if(addr == 0)
//         return 0;
//       ip->addrs[bn] = addr;
//     }
//     return addr;
//   }
//   bn -= NDIRECT;

//   if(bn < NINDIRECT){
//     // Load indirect block, allocating if necessary.
//     //NOTE - addrs[NDIRECT] is the last element, which holds the block number of the INDIRECT block.
//     // Then it goes into the indirect block, and search for the index bn-NDIRECT. Allocate if empty.
//     if((addr = ip->addrs[NDIRECT]) == 0){
//       //ANCHOR[id=balloc_ex_1]
//       addr = balloc(ip->dev);
//       if(addr == 0)
//         return 0;
//       ip->addrs[NDIRECT] = addr;
//     }
//     //Each buf* has BSIZE of uchar in buf->data.
//     //And each blockn is an uint (4 bytes), so in total 256 blockns.
//     bp = bread(ip->dev, addr);
//     a = (uint*)bp->data;
//     if((addr = a[bn]) == 0){
//       //ANCHOR[id=balloc_ex_2]
//       addr = balloc(ip->dev);
//       if(addr){
//         a[bn] = addr;
//         log_write(bp);
//       }
//     }
//     brelse(bp);
//     return addr;
//   }

//   panic("bmap: out of range");
// }

static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, addr1, addr2, *a, *a1, *a2;
  struct buf *bp, *bp1, *bp2;

  // printf("bn: %d\n", bn);

  if(bn < NDIRECT)
  {
    if((addr = ip->addrs[bn]) == 0)
    {
      addr = balloc(ip->dev);
      if(addr == 0)
        return 0;
      ip->addrs[bn] = addr;
    }
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT)
  {
    // Load indirect block, allocating if necessary.
    //NOTE - addrs[NDIRECT] is the last element, which holds the block number of the INDIRECT block.
    // Then it goes into the indirect block, and search for the index bn-NDIRECT. Allocate if empty.
    if((addr = ip->addrs[NDIRECT]) == 0)
    {
      //ANCHOR[id=balloc_ex_1]
      addr = balloc(ip->dev);
      if(addr == 0)
        return 0;
      ip->addrs[NDIRECT] = addr;
    }
    //Each buf* has BSIZE of uchar in buf->data.
    //And each blockn is an uint (4 bytes), so in total 256 blockns.
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0)
    {
      //ANCHOR[id=balloc_ex_2]
      addr = balloc(ip->dev);
      if(addr)
      {
        a[bn] = addr;
        log_write(bp);
      }
    }
    brelse(bp);
    return addr;
  }
  //Skip the 256 direct blocks as well
  bn -= NINDIRECT;
  
  //if(bn >= NINDIRECT && bn < NINDIRECT * NINDIRECT + NINDIRECT + NDIRECT)
  if(bn < NINDIRECT * NINDIRECT)
  {
    // printf("bigfile\n");
    //double indirect - first indirect
    if((addr = ip->addrs[NDIRECT+1]) == 0)
    {
      addr = balloc(ip->dev);
      if(addr == 0)
        return 0;
      ip->addrs[NDIRECT+1] = addr;
    }
    bp1 = bread(ip->dev, addr);
    a1 = (uint*)bp1->data;
    //e.g. let's say bn-11=1989, 1989-256=1733
    //first indirect blockn=1733/256-1
    if((addr1 = a1[(uint)(bn / NINDIRECT)]) == 0)
    {
      addr1 = balloc(ip->dev);
      if(addr1)
      {
        a1[(uint)(bn / NINDIRECT)] = addr1;
        log_write(bp1);
      }
    }
    brelse(bp1);
    //double indirect - second indirect
    //now we have addr1
    bp2 = bread(ip->dev, addr1);
    a2 = (uint*)bp2->data;
    //second indirect blockn=1989%256-1=196
    if((addr2 = a2[bn % NINDIRECT]) == 0)
    {
      addr2 = balloc(ip->dev);
      if(addr2)
      {
        a2[(uint)(bn % NINDIRECT)] = addr2;
        log_write(bp2);
      }
    }
    brelse(bp2);
    return addr2;
  }

  panic("bmap: out of range");
}

// Truncate inode (discard contents).
// Caller must hold ip->lock.
void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp;
  uint *a;

  //NOTE - Start with direct blocks
  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]);
      //NOTE - 0 means empty - future balloc() will allocate
      //LINK - kernel/fs.c#balloc_ex_1
      //LINK - kernel/fs.c#balloc_ex_2
      ip->addrs[i] = 0;
    }
  }

  //NOTE - Single indirect blocks
  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      //TODO: Why not set a[j]=0?
      if(a[j])
        bfree(ip->dev, a[j]);
    }
    brelse(bp);
    //NOTE - The indirect block itself
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

  //Double indirect blocks
  if(ip->addrs[NDIRECT+1])
  {
    bp = bread(ip->dev, ip->addrs[NDIRECT+1]);
    a = (uint*)bp->data;
    //a[j] is indirect, don't bfree until the second layer is done
    for(j = 0; j < NINDIRECT; j++)
    {
      if(a[j])
      {
        //Recall the 2nd argument of bread() is blockno
        struct buf *bp2 = bread(ip->dev, a[j]);
        uint *a2 = (uint*)bp2->data;
        for(int k = 0; k < NINDIRECT; k++)
        {
          //Free layer-2 direct blocks, 256 for each layer-1 block
          if(a2[k])
            bfree(ip->dev, a2[k]);
        }
        brelse(bp2);
        //Free layer-1 indirect blocks
        bfree(ip->dev, a[j]);
      }
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT+1]);
    ip->addrs[NDIRECT+1] = 0;
  }

  ip->size = 0;
  iupdate(ip);

  //NOTE - caller should call releasesleep(&ip->lock) to release ip->lock
}

// Copy stat information from inode.
// Caller must hold ip->lock.
//NOTE - Used by the stat syscall
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

// Read data from inode.
// Caller must hold ip->lock.
// If user_dst==1, then dst is a user virtual address;
// otherwise, dst is a kernel address.
//NOTE - Copy n bytes, starting from offset off, from ip, to VA user_dst
//We have two error code: 0 and -1
int
readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  //NOTE - If we want to read beyond the end of the file (off > ip->size),
  //or if we want to read negative number of bytes (off + n < off),
  //return an error (0 bytes read)
  if(off > ip->size || off + n < off)
    return 0;
  //NOTE - if part of the address we want to read lies beyond the end of the file,
  // it truncates it to make sure the read doesn't cross the end of the file.
  if(off + n > ip->size)
    n = ip->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    //NOTE - Get the block number
    uint addr = bmap(ip, off/BSIZE);
    if(addr == 0)
      break;
    //NOTE - Use the block number to fetch a pointer to the buffer
    bp = bread(ip->dev, addr);
    /*NOTE - What is the maximum number of bytes readi() reads/copies out each loop?
      BSIZE = 1024, tot = 0 for first loop
      Let's say we have a file of 4,096 bytes, and want to read 4,096 bytes from offset 0.
      First loop: n - tot = 4,096, BSIZE - off%BSIZE = 1,024, so m = 1,024 bytes
      Apparently, BSIZE - off%BSIZE is capped at BSIZE, so m is capped at BSIZE bytes
    */
    m = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1) {
      brelse(bp);
      tot = -1;
      break;
    }
    brelse(bp);
  }
  return tot;
}

// Write data to inode.
// Caller must hold ip->lock.
// If user_src==1, then src is a user virtual address;
// otherwise, src is a kernel address.
// Returns the number of bytes successfully written.
// If the return value is less than the requested n,
// there was an error of some kind.
int
writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  //NOTE - If we want to start the write beyond the end of file (off > ip->size),
  //or want to write negative number of bytes (off + n < off),
  //return -1 (why not 0? probably because we can write 0 bytes)
  if(off > ip->size || off + n < off)
    return -1;
  //NOTE - Cannot write pass maximum file size
  if(off + n > MAXFILE*BSIZE)
    return -1;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    //NOTE - Get the block number
    uint addr = bmap(ip, off/BSIZE);
    if(addr == 0)
      break;
    //NOTE - Use the block number to fetch a pointer to the buffer
    bp = bread(ip->dev, addr);
    m = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyin(bp->data + (off % BSIZE), user_src, src, m) == -1) {
      brelse(bp);
      break;
    }
    //NOTE - log_write() doesn't write into disk. It just bpin() bp.
    log_write(bp);
    //NOTE - No longer needs the buffer??
    brelse(bp);
  }

  //NOTE - Extend the file size if needed (off is INCREMENTED in the for loop)
  if(off > ip->size)
    ip->size = off;

  // write the i-node back to disk even if the size didn't change
  // because the loop above might have called bmap() and added a new
  // block to ip->addrs[].
  iupdate(ip);

  return tot;
}

// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
//NOTE SOME callers of dirlookup() lock dp first, not sure if it's every caller, though.
//LINK - kernel/fs.c#lock_dp_1
/*TODO - Figure out this part of the text:
  (why dirlookup() returns dp unlocked)
  The caller has locked dp, so if the lookup was for ., an alias for the current directory, 
  attempting to lock the inode before returning would try to re-lock dp and deadlock.

  But I don't get what's so special with .? The deadlock would happen to everything, no?
*/
//NOTE - I have tracked this function by bp @ sys_open().
//So dirlookup() goes through every item in the directory, starting from . and ..
//For each item, it uses namecmp() to match the names
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  //NOTE - directories are implemented like a file. Its inode has type = T_DIR.
  //Its data is a sequence of directory entries
  //TODO - Where is the definition of a sample directory object?
  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  //NOTE - directories are like files, so we can use readi() to read into a struct dirent
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      // entry matches path element
      //NOTE - Save byte offset of the entry in case caller wants to edit
      if(poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
// Returns 0 on success, -1 on failure (e.g. out of disk blocks).
//TODO - Figure out where does inum come from. Does the caller increment it?
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    //NOTE - dirlookup() calls iget() which increases ref, so call iput() to decrement ref
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  //NOTE - Each "directory" block contains BSIZE/sizeof(de) of struct dirent
  //in its data field
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  //NOTE - Write back to the directory inode
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    return -1;

  return 0;
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  /*NOTE - Look at the first character of path:
  1. path starts with '/', which means it's a root path (e.g. /dev/null in Linux)
  2. path doesn't start with '/', get the current working directory
  //TODO - I don't know, what about other paths, e.g. path starts with .. ?
  */
  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(myproc()->cwd);

  //NOTE - skipelem() copies the next path element from path into name
  //e.g. skipelem("///a//bb", name) = "bb", setting name = "a"
  //e.g. skipelem("a", name) = "", setting name = "a"
  while((path = skipelem(path, name)) != 0){
    //ANCHOR[id=lock_dp_1]
    ilock(ip);
    //NOTE - Must be wrong if it's not a directory inode
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    //NOTE - If want parent dir AND we exhausted all / chars in path (see second e.g. ^)
    if(nameiparent && *path == '\0'){
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    //TODO - If path = "./testdir1", in the first loop, 
    //path is now "testdir1" and name is now "."
    //why does it assume that there is always a name? 
    //I need to debug more to find out
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    //NOTE - next is now the directory entry matching `name` in `ip`
    //Prepare for next while iteration
    //NOTE - Prevent deadlock: If we are looking up . , 
    // that means the original ip locked by ilock(ip) ^ is the same as next,
    // to prevent deadlock, we use iunlockput(ip) to release the lock
    ip = next;
  }
  /*TODO - Whence we reach this point, it means:
    - The original ip is indeed a directory (no error return)
    - ???
  */
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}
