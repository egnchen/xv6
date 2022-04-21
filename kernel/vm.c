#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "fcntl.h"
#include "fs.h"
#include "file.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // map kernel stacks
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// Initialize the one kernel_pagetable
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if(size == 0)
    panic("mappages: size");
  
  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

// lookup for vma_region in vma region list
// return vma_region with lock held
// or NULL if not found
struct vma_region *
vma_lookup(struct proc *p, uint64 addr)
{
  struct vma_region *vma = p->vma;
  while(vma) {
    acquire(&vma->lock);
    if(addr >= vma->addr && addr <= vma->addr + vma->length) {
      break;
    }
    release(&vma->lock);
    vma = vma->next;
  }
  return vma;
}

int
munmap(struct proc *p, const uint64 addr, const int length)
{
  struct vma_region *vma = 0;
  // borrowed from filewrite
  const int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;

  // walk through the range
  int left = max; // batching optimization

  // the locking here is a bit obscure
  // because we can't hold a lock while doing I/O
  // for convenience vma_region locks are only held when they're removed
  // this means concurrent munmap calls might be dangerous
  begin_op();
  for(uint64 va = addr; va < addr + length; va += PGSIZE) {
    if(!vma || va < vma->addr || va > vma->addr + vma->length) {
      // wrong region, find another one
      if(vma) {
        // should we release last one?
        acquire(&vma->lock);
        if(addr <= vma->addr && addr + length >= vma->addr + vma->length) {
          vma_remove(p, vma);
        } else {
          release(&vma->lock);
        }
      }
      vma = vma_lookup(p, addr);
      if(!vma) {
        printf("munmap: addr %p not found\n", addr);
        return -1;
      }
      release(&vma->lock);
      // start a new fs operation
      end_op();
      left = max;
      begin_op();
    }
    pte_t *pte = walk(p->pagetable, va, 0);
    if(!pte || (*pte & PTE_V) == 0) {
      // not mapped, nothing to do
      continue;
    }
    if(vma->flags & MAP_SHARED && (*pte & PTE_D)) {
      // dirty, write back
      int len = PGSIZE;
      if(va + len > vma->addr + vma->length) {
        len = vma->addr + vma->length - addr;
      }
      if(len > left) {
        // start a new fs operation
        end_op();
        left = max;
        begin_op();
      }
      ilock(vma->f->ip);
      writei(vma->f->ip, 1, va, va - vma->addr + vma->offset, len);
      iunlock(vma->f->ip);
      left -= len;
    }
    // unmap this page
    uvmunmap(p->pagetable, va, 1, 1);
  }
  end_op();

  // should we release the last one?
  if(vma){
    acquire(&vma->lock);
    if(addr <= vma->addr && addr + length >= vma->addr + vma->length) {
      vma_remove(p, vma);
    } else {
      release(&vma->lock);
    }
  }
  
  return 0;
}

uint64
mmap(struct proc *p, uint64 addr, int length,
      int prot, int flags, struct file *f, int offset)
{
  // first, get a new vma region
  struct vma_region *vma = vma_alloc();
  if(vma == 0) {
    return -1;
  }

  if(addr == 0) {
    // default address is either VMA_ADDR_START or after last vma_region
    addr = VMA_ADDR_START;
    if(p->vma) {
      addr = PGROUNDUP(p->vma->addr + p->vma->length);
    }
  } else {
    // pick a nearby page boundary
    // this behavior is same as linux
    addr = PGROUNDUP(addr);
  }
  
  if((flags & MAP_SHARED) && (prot & PROT_WRITE) && !f->writable) {
    goto bad;
  }

  vma->addr = addr;
  vma->length = length;
  vma->prot = prot;
  vma->flags = flags;
  vma->f = filedup(f);
  vma->offset = offset;

  vma_add(p, vma);

  release(&vma->lock);
  return vma->addr;
bad:
  release(&vma->lock);
  return -1;
}

int
handle_mmap(struct proc *p, uint64 scause, uint64 addr)
{
  struct vma_region *vma = vma_lookup(p, addr);
  if(!vma) {
    // given vma region not found
    printf("handle_mmap: addr %p not found\n", addr);
    return -1;
  }

  // read & map a page
  uint64 pte_perm = PTE_U;
  uint64 kpage;
  if((scause == 12 && !(vma->prot & PROT_EXEC)) ||
      (scause == 13 && !(vma->prot & PROT_READ)) ||
      (scause == 15 && !(vma->prot & PROT_WRITE))) {
    printf("error handle_mmap: scause = %d, prot = %d\n", scause, vma->prot);
    goto bad;
  }
  if(vma->f == 0) {
    panic("handle_mmap: no file");
    goto bad;
  }
  // do mapping
  kpage = (uint64)kalloc();
  if(vma->prot & PROT_READ)
    pte_perm |= PTE_R;
  if(vma->prot & PROT_WRITE)
    pte_perm |= PTE_W;
  if(vma->prot & PROT_EXEC)
    pte_perm |= PTE_X;
  if(mappages(p->pagetable, PGROUNDDOWN(addr), PGSIZE, kpage, pte_perm) < 0) {
    panic("handle_mmap: mappages");
    goto bad;
  }
  // read content
  // in user-space we're writing @ PGROUNDDOWN(addr)
  // in kernel-space we're writing @ kpage
  struct inode *ip = vma->f->ip;
  uint64 addr_start = kpage;  // this is kernel pointer
  int read_offset = vma->offset + PGROUNDDOWN(addr) - vma->addr;
  int left = PGSIZE;
  ilock(ip);
  for(int read_count; (read_count = readi(ip, 0, addr_start, read_offset, left)); ) {
    addr_start += read_count;
    read_offset += read_count;
    left -= read_count;
  }
  iunlock(ip);
  if(left) {
    memset((void *)addr_start, 0, left);
  }
  release(&vma->lock);
  return 0;
bad:
  release(&vma->lock);
  return -1;
}