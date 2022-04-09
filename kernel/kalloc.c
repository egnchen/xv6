// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"

void freerange(int cpuid, void *pa_start, void *pa_end);
void stealmem(int me);
void kfree_locked(int cpuid, void *pa);

// prefetch 1MB
static const int PREFETCH_PAGE_CNT = 256;

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock mainlock;
  struct spinlock locks[NCPU];
  struct run *freelist[NCPU];
  int freecnt[NCPU];
} kmem;

static char lock_names[NCPU][7];

void
kinit()
{
  uint64 pa_start = PGROUNDUP((uint64)end);
  uint64 pa_end = PHYSTOP;
  uint64 mem_len = pa_end - pa_start;

  initlock(&kmem.mainlock, "kmem-main");
  
  for(int i = 0; i < NCPU; i++) {
    snprintf(lock_names[i], 6, "kmem-%d", i);
    initlock(kmem.locks + i, lock_names[i]);
    kmem.freecnt[i] = 0;

    void *cur_start = (void *)(pa_start + i * mem_len / NCPU);
    void *cur_end = (void *)(pa_start + (i + 1) * mem_len / NCPU + 1);
    freerange(i, cur_start, cur_end);
  }
}

// add [ROUND_UP(pa_start), pa_end) to freelist #cpuid
void
freerange(int cpuid, void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  acquire(&kmem.locks[cpuid]);
  for(; p + PGSIZE < (char*)pa_end; p += PGSIZE)
    kfree_locked(cpuid, p);
  release(&kmem.locks[cpuid]);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  push_off();
  int id = cpuid();
  acquire(&kmem.locks[id]);
  kfree_locked(id, pa);
  release(&kmem.locks[id]);
  pop_off();
}

// free page with per-cpu lock held
void
kfree_locked(int cpuid, void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
  r->next = kmem.freelist[cpuid];
  kmem.freelist[cpuid] = r;
  kmem.freecnt[cpuid]++;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  int id;

  push_off();
  id = cpuid();
  acquire(&kmem.locks[id]);

  r = kmem.freelist[id];
  if(!r) {
    // slow-path: try to steal memory from another CPU
    int try = 5;
    while(!r && try--) {
      release(&kmem.locks[id]);
      stealmem(id);
      acquire(&kmem.locks[id]);
      r = kmem.freelist[id];
    }
    if(!r) {
      printf("%d: no mem available\n", id);
      goto end;
    }
  }

  kmem.freelist[id] = r->next;
  kmem.freecnt[id]--;

end:
  release(&kmem.locks[id]);
  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}


// steal memory for *me* from another CPU
void
stealmem(int me)
{
  struct run *r, *rstart;
  int cnt;
  // find the CPU node with most memory
  int victim = 0;
  for(int i = 0; i < NCPU; i++) {
    if(i != me && kmem.freecnt[i] > kmem.freecnt[victim]) {
      victim = i;
    }
  }

  // we steal at most PREFETCH_PAGE_CNT pages from victim
  acquire(&kmem.locks[victim]);

  rstart = r = kmem.freelist[victim];
  if(!r) {
    release(&kmem.locks[victim]);
    return;
  }
  cnt = 1;
  while(r->next && cnt < PREFETCH_PAGE_CNT) {
    r = r->next;
    cnt++;
  }
  // part we want to steal is [rstart, r]
  kmem.freelist[victim] = r->next;
  kmem.freecnt[victim] -= cnt;

  release(&kmem.locks[victim]);

  // hand it over
  acquire(&kmem.locks[me]);
  r->next = kmem.freelist[me];
  kmem.freelist[me] = rstart;
  kmem.freecnt[me] += cnt;
  release(&kmem.locks[me]);
}