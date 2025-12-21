// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "bio.h"
#include "buf.h"

//ANCHOR[id=buf_list]
// struct bc {
//   struct spinlock lock;
//   struct buf buf[NBUF];

//   // Linked list of all buffers, through prev/next.
//   // Sorted by how recently the buffer was used.
//   // head.next is most recent, head.prev is least.
//   struct buf head;
// } bcache;

struct bc bcache;

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  // Create linked list of buffers
  /*NOTE - Circular double linked list
    Assuming two elements in buf[]
          ______         ____         ____
    |-----|head|<--next--|b0|<--next--|b1|<----------|
    | --->|____|--prev-->|__|--prev-->|__|---prev--| |
    | |                                            | |
    | |--------------------------------------------| |
    |                                                |
    |--next-->---------------------------------------|
  */
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    initsleeplock(&b->lock, "buffer");
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  /*NOTE -  Hold the lock early to ensure there is only one cached buffer per each disk sector,
            and readers see writes
  */
  acquire(&bcache.lock);

  // Is the block already cached?
  for(b = bcache.head.next; b != &bcache.head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock);
      /*NOTE - Two locks
      1. bcache.lock protects the metadata of the buffer (refcnt, dev, valid, etc.)
      2. sleeplock b->lock protects read/write of the block's buffered content
      Looks like read/write must releases the sleeplock by calling brelse()
      //LINK - kernel/fs.c#brelse_example
      */
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  //NOTE - it doesn't load from disk, just return the buffer.
  //LINK - kernel/bio.c#load_from_disk
  for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    //ANCHOR[id=load_from_disk]
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.lock);
  //NOTE - Food for thought: What happens if it reboots immediately after the next line?
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    /*NOTE - Put b to the position of head, and push head one slot further
    Assuming we have the following setup in buf[], and we want to brelse() b1
          ______         ____         ____         ____
    |-----|head|<--next--|b0|<--next--|b1|<--next--|b2|<---------|
    | --->|____|--prev-->|__|--prev-->|__|--prev-->|__|--prev->| |
    | |                                                        | |
    | |--------------------------------------------------------- |
    |                                                            |
    |--next-->---------------------------------------------------|

    b->next->prev = b->prev;      | b0->prev = b2
    b->prev->next = b->next;      | b2->next = b0
    b->next = bcache.head.next;   | b1->next = b2
    b->prev = &bcache.head;       | b1->prev = head
    bcache.head.next->prev = b;   | b2->prev = b1
    bcache.head.next = b;         | head.next = b1

    So we have the following setup afterwards:

          ____         ______         ____         ____
    |-----|b1|<--next--|head|<--next--|b0|<--next--|b2|<---------|
    | --->|__|--prev-->|____|--prev-->|__|--prev-->|__|--prev->| |
    | |                                                        | |
    | |--------------------------------------------------------- |
    |                                                            |
    |--next-->---------------------------------------------------|

    */
    b->next->prev = b->prev;
    b->prev->next = b->next;    // These two lines decouple b->next and b->prev from b
    b->next = bcache.head.next; // Part I of the process to wire b to tail
    b->prev = &bcache.head;     // Part I of the process to wire original head to b1
    bcache.head.next->prev = b; // Part II of the process to wire b to tail
    bcache.head.next = b;       // Part II of the process to wire original head to b1
  }
  
  release(&bcache.lock);
}

void
bpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

void
bunpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}


