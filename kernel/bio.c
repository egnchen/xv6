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

#define NBUCKET 13

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // hash table optimization
  struct buf *htable[NBUCKET];
  struct spinlock htlock[NBUCKET];
} bcache;

void
print_bcache()
{
  printf("hash table:\n");
  for(int i = 0; i < NBUCKET; i++) {
    printf("%d\t", i);
    for(struct buf *t = bcache.htable[i]; t; t = t->next) {
      printf("%d(%p) ", t->blockno, t);
    }
    printf("\n");
  }
}

static inline uint
hkey(uint dev, uint blockno)
{
  return ((dev + 1) * blockno) % NBUCKET;
}

// remove buffer from linked-list in hash table
// this function should be called with bucket list held
static inline void
hremove(struct buf *victim, struct buf **bucket)
{
  // bucket is the pointer to next pointer
  while(*bucket) {
    if(*bucket == victim) {
      *bucket = victim->next;
      break;
    }
    bucket = &((*bucket)->next);
  };
  // list empty / not found / found & removed
  victim->next = 0;
}

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache.flist");
  // initialize buffers
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->refcnt = 0;
    b->next = 0;
    b->timestamp = 0;
    initsleeplock(&b->lock, "buffer");
  }
  // Create hash table of buffers
  for(int i = 0; i < NBUCKET; i++) {
    bcache.htable[i] = 0;
    initlock(&bcache.htlock[i], "bcache.bucket");
  }
  // We put those buffers randomly into each bucket
  // and mark them as free
  for(b = bcache.buf; b < bcache.buf+NBUF; b++) {
    uint key = (b - bcache.buf) * NBUCKET / NBUF;
    acquire(&bcache.htlock[key]);
    b->next = bcache.htable[key];
    bcache.htable[key] = b;
    release(&bcache.htlock[key]);
  }
  print_bcache();
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
struct buf*
bget(uint dev, uint blockno)
{
  struct buf *ret;
  struct buf *victim;
  uint key = hkey(dev, blockno);
  int lock_released = 0;

  // get bucket lock
  acquire(&bcache.htlock[key]); 

  // is the block already cached?
  // if it is, return directly
  for(ret = bcache.htable[key]; ret; ret = ret->next) {
    if(ret->dev == dev && ret->blockno == blockno)
      break;
  }
  if(ret) {
    ret->refcnt++;
    goto ok;
  }

  // get a free buf from current bucket first
  victim = bcache.htable[key];
  for(struct buf *b = victim; b; b = b->next) {
    if(b->refcnt == 0 && b->timestamp < victim->timestamp) {
      victim = b;
    }
  }

  if(victim && victim->refcnt == 0) {
    // found our victim
    hremove(victim, &bcache.htable[key]);
  } else {
    // we still have to get a victim from another bucket
    release(&bcache.htlock[key]);
    lock_released = 1;

    for(uint ik = 0; ik < NBUCKET; ik++) {
      acquire(&bcache.htlock[ik]);
      victim = bcache.htable[ik];
      for(struct buf *b = victim; b; b = b->next) {
        if(b->refcnt == 0 && b->timestamp < victim->timestamp) {
          victim = b;
        }
      }
      if(victim && victim->refcnt == 0) {
        hremove(victim, &bcache.htable[ik]);
        release(&bcache.htlock[ik]);
        acquire(&bcache.htlock[key]);
        break;
      }
      release(&bcache.htlock[ik]);
    }

    if(!victim || victim->refcnt > 0) {
      // went through all and still nothing :(
      print_bcache();
      panic("bget: no free buf");
    }
  }

  victim->dev = dev;
  victim->blockno = blockno;
  victim->refcnt = 1;
  victim->valid = 0;
  ret = victim;

  if(lock_released) {
    // It is possible that in between another thread swoop in and
    // insert another identical buffer. Check for it.
    for(struct buf *b = bcache.htable[key]; b; b = b->next) {
      if(b->refcnt > 0 && b->dev == dev && b->blockno == blockno) {
        // Jackpot! Invalidate the victim.
        victim->refcnt = 0;
        ret = b;
      }
    }
  }

  // fetch new one & add it to hash table
  victim->next = bcache.htable[key];
  bcache.htable[key] = victim;

ok:
  release(&bcache.htlock[key]);
  acquiresleep(&ret->lock);
  // printf("%d bget (%d,%d) -> %p\n", cpuid(), dev, blockno, b);
  return ret;
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
void
brelse(struct buf *b)
{
  uint key = hkey(b->dev, b->blockno);
  if(!holdingsleep(&b->lock))
    panic("brelse");

  acquire(&bcache.htlock[key]);
  b->refcnt--;
  if(b->refcnt == 0) {
    // see kernel/trap.c
    b->timestamp = ticks;
  }
  release(&bcache.htlock[key]);
  releasesleep(&b->lock);
}

void
bpin(struct buf *b) {
  uint key = hkey(b->dev, b->blockno);
  acquire(&bcache.htlock[key]);
  b->refcnt++;
  release(&bcache.htlock[key]);
}

void
bunpin(struct buf *b) {
  uint key = hkey(b->dev, b->blockno);
  acquire(&bcache.htlock[key]);
  b->refcnt--;
  release(&bcache.htlock[key]);
}


