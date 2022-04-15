// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

static struct spinlock reflock;
// page refcnts
// 32-bit integer is used here to support atomic operations
// this is a matter of space-time tradeoff
// since 8-bit is well enough for most cases
static uint32 refarray[(PHYSTOP - KERNBASE) / PGSIZE];
#define REFCNT(pa) (refarray[((uint64)(pa) - KERNBASE) / PGSIZE])

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&reflock, "kmem_reflock");
  for(int i = 0; i < sizeof(refarray) / sizeof(uint32); i++)
    refarray[i] = 1;
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree: out of range");
  
  if(REFCNT(pa) != 1) {
    printf("%p %d\n", pa, REFCNT(pa));
    panic("kfree: ref");
  }
  
  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  // clear reference
  REFCNT(pa) = 0;

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r) {
    memset((char*)r, 5, PGSIZE); // fill with junk
    REFCNT(r) = 1;
  }
  return (void*)r;
}

// Collect the amount of free memory
uint64
kgetfree()
{
  int page_cnt = 0;

  // TODO maybe locking is required here?
  struct run *r = kmem.freelist;
  while(r) {
    page_cnt++;
    r = r->next;
  }

  return page_cnt * PGSIZE;
}

void
krefacquire()
{
  acquire(&reflock);
}

void
krefrelease()
{
  release(&reflock);
}

uint32
krefinc(uint64 pa)
{
  return __sync_fetch_and_add(&REFCNT(pa), 1);
}

uint32
krefdec(uint64 pa)
{
  return __sync_fetch_and_sub(&REFCNT(pa), 1);
}

uint32
kref(uint64 pa)
{
  return REFCNT(pa);
}