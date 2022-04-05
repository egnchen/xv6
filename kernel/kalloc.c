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

static uint8 refarray[(PHYSTOP - KERNBASE) / PGSIZE];
#define REFCNT(pa) (refarray[((uint64)(pa) - KERNBASE) / PGSIZE])

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  memset(refarray, 0, sizeof(refarray));
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
  
  if(REFCNT(pa) != 1)
    panic("kfree: ref");
  
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

void
krefadd(uint64 pa)
{
  if(REFCNT(pa) == __UINT8_MAX__) {
    panic("krefadd");
  }
  REFCNT(pa)++;
}

void
krefdrop(uint64 pa)
{
  if(REFCNT(pa) == 0) {
    panic("krefdrop");
  }
  REFCNT(pa)--;
}

uint8
kref(uint64 pa)
{
  return REFCNT(pa);
}