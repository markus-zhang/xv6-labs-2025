#ifndef _BIO_H_
#define _BIO_H_

#include "spinlock.h"
#include "param.h"
#include "buf.h"

struct bc {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;
};

#endif