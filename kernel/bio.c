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
  return ((dev + 1) * blockno) % NBUCKET;
}

static inline uint
get_fl_len()
{
  int ret = 0;
  for(struct buf *b = bcache.fhead.prev; b != &bcache.fhead; b = b->prev) {
    ret++;
  }
  return ret;
}

void
print_bcache()
{
  printf("hash table:\n");
  for(int i = 0; i < NBUCKET; i++) {
    printf("%d\t", i);
    for(struct buf *t = bcache.htable[i]; t; t = t->hnext) {
      printf("%d(%p) ", t->blockno, t);
    }
    printf("\n");
  }

  // and free list
  printf("free list:\n");
  for(struct buf *b = bcache.fhead.next; b != &bcache.fhead; b = b->next) {
    printf("%d(%p) ", b->blockno, b);
  }
  printf("\n");
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
    b->refcnt = 0;
    b->hnext = 0;
    initsleeplock(&b->lock, "buffer");
    b->next = bcache.fhead.next;
    b->prev = &bcache.fhead;
    bcache.fhead.next->prev = b;
    bcache.fhead.next = b;
  }
  for(int i = 0; i < NBUCKET; i++) {
    bcache.htable[i] = 0;
    initlock(&bcache.htlock[i], "bcache.bucket");
  }
  print_bcache();
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  uint key = bkey(dev, blockno);

  // get bucket lock
  acquire(&bcache.htlock[key]); 

  // is the block already cached?
  for(b = bcache.htable[key]; b; b = b->hnext) {
    if(b->dev == dev && b->blockno == blockno)
      break;
  }

  // if it is, return directly
  if(b) {
    b->refcnt++;
    goto ok;
  }

  // we **don't** drop htable lock here

  // we need a new one from free list
  acquire(&bcache.lock);
  // uint llen = get_fl_len();
  b = bcache.fhead.prev;
  if(b == &bcache.fhead) {
    print_bcache();
    panic("bget: no free buf");
  }
  b->next->prev = b->prev;
  b->prev->next = b->next;
  // printf("bget: len %d->%d\n", llen, get_fl_len());
  release(&bcache.lock);
  b->next = b->prev = 0;

  // update metadata & drop into htable
  if(b->refcnt != 0) {
    panic("bget: not free");
  }
  b->dev = dev;
  b->blockno = blockno;
  b->refcnt = 1;
  b->valid = 0;
  b->hnext = bcache.htable[key];
  bcache.htable[key] = b;

ok:
  release(&bcache.htlock[key]);
  acquiresleep(&b->lock);
  // printf("%d bget (%d,%d) -> %p\n", cpuid(), dev, blockno, b);
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
  uint key = bkey(b->dev, b->blockno);
  if(!holdingsleep(&b->lock))
    panic("brelse");

  // printf("%d brelse (%d,%d,%d) -> %p\n", cpuid(), b->dev, b->blockno, b->refcnt, b);
  acquire(&bcache.htlock[key]);
  b->refcnt--;
  if(b->refcnt == 0) {
    // no longer referenced
    // remove it from hash table
    if(bcache.htable[key] == 0) {
      panic("brelse: buf already freed");
    }
    if(bcache.htable[key] == b) {
      bcache.htable[key] = b->hnext;
    } else {
      struct buf *prev;
      for(prev = bcache.htable[key]; prev->hnext; prev = prev->hnext) {
        if(prev->hnext == b) break;
      }
      if(prev->hnext) {
        prev->hnext = b->hnext;
      } else {
        panic("brelse: buf already freed");
      }
    }
    b->hnext = 0;

    // add it back to free list
    acquire(&bcache.lock);
    // uint llen = get_fl_len();
    b->next = bcache.fhead.next;
    b->prev = &bcache.fhead;
    bcache.fhead.next->prev = b;
    bcache.fhead.next = b;
    // printf("brelse: len %d->%d\n", llen, get_fl_len());
    release(&bcache.lock);
  }
  release(&bcache.htlock[key]);

  releasesleep(&b->lock);
}

void
bpin(struct buf *b) {
  uint key = bkey(b->dev, b->blockno);
  acquire(&bcache.htlock[key]);
  b->refcnt++;
  release(&bcache.htlock[key]);
}

void
bunpin(struct buf *b) {
  uint key = bkey(b->dev, b->blockno);
  acquire(&bcache.htlock[key]);
  b->refcnt--;
  release(&bcache.htlock[key]);
}


