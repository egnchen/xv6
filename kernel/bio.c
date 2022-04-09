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
#include "buf.h"

#define NBUCKET 17

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  struct buf fhead;   // Linked list of free buffers

  // hash table optimization
  struct buf *htable[NBUCKET];
  struct spinlock htlock[NBUCKET];
} bcache;

inline static uint
bkey(uint dev, uint blockno)
{
  return (dev * blockno) % NBUCKET;
}

inline static void
addbuf(struct buf *b)
{
  uint key = bkey(b->dev, b->blockno);
  // printf("adding buf\t%d\t%d,%d(%p)\n", key, b->dev, b->blockno, b);
  struct buf *t;
  acquire(&bcache.htlock[key]);
  t = bcache.htable[key];
  b->hnext = t;
  bcache.htable[key] = b;
  release(&bcache.htlock[key]);
}

inline static int
removebuf(struct buf *b)
{
  uint key = bkey(b->dev, b->blockno);
  // printf("removing buf\t%d\t%d,%d(%p)\n", key, b->dev, b->blockno, b);
  struct buf *p;
  acquire(&bcache.htlock[key]);
  p = bcache.htable[key];
  if(!p)
    goto fail;
  if(p == b) {
    bcache.htable[key] = p->hnext;
    p->hnext = 0;
    goto ok;
  }
  while(p->hnext && p->hnext != b)
    p = p->hnext;
  if(!p)
    goto fail;
  if(p->hnext == b) {
    p->hnext = b->hnext;
    b->hnext = 0;
    goto ok;
  }
ok:
  release(&bcache.htlock[key]);
  return 0;
fail:
  panic("removebuf: not found");
}

inline static struct buf *
findbuf(uint dev, uint blockno)
{
  struct buf *ret;
  uint key = bkey(dev, blockno);
  acquire(&bcache.htlock[key]);
  ret = bcache.htable[key];
  while(ret && (ret->dev != dev || ret->blockno != blockno)) {
    ret = ret->hnext;
  }
  release(&bcache.htlock[key]);
  return ret;
}

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");
  // Create linked list of buffers
  bcache.fhead.next = &bcache.fhead;
  bcache.fhead.prev = &bcache.fhead;
  // all free at first
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.fhead.next;
    b->prev = &bcache.fhead;
    b->hnext = 0;
    initsleeplock(&b->lock, "buffer");
    bcache.fhead.next->prev = b;
    bcache.fhead.next = b;
  }
  for(int i = 0; i < NBUCKET; i++) {
    bcache.htable[i] = 0;
    initlock(&bcache.htlock[i], "bcache.bucket");
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  // Is the block already cached?
  b = findbuf(dev, blockno);
  if(b) {
    b->refcnt++;
    acquiresleep(&b->lock);
    return b;
  }

  // Not cached.
  // just take the last one from free list, add it to used list
  acquire(&bcache.lock);

  b = bcache.fhead.prev;
  if(!b) {
    panic("bget: no buffers");
  }
  if(b->refcnt != 0) {
    panic("bget: not free");
  }
  b->next->prev = b->prev;
  b->prev->next = b->next;
  b->next = b->prev = 0;

  release(&bcache.lock);

  b->dev = dev;
  b->blockno = blockno;
  b->valid = 0;
  b->refcnt = 1;
  addbuf(b);

  acquiresleep(&b->lock);
  
  return b;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
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
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    removebuf(b);
    b->next = bcache.fhead.next;
    b->prev = &bcache.fhead;
    bcache.fhead.next->prev = b;
    bcache.fhead.next = b;
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


