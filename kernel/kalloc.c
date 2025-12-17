// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

#define NSUPER 8

void freerange(void *pa_start, void *pa_end);
void superfree(void *pa);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
  struct run *superfreelist; // 2MB 块 freelist
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  kmem.freelist = 0;
  kmem.superfreelist = 0;

  uint64 pa_start = PGROUNDUP((uint64)end);
  uint64 pa_end   = PHYSTOP;

  // 2MB-aligned start for superpages
  uint64 super_start = (pa_start + SUPERPGSIZE - 1) & ~(SUPERPGSIZE - 1);
  uint64 super_end   = super_start + (uint64)NSUPER * SUPERPGSIZE;

  if(super_end > pa_end)
    panic("kinit: not enough memory for superpages");

  // 1) normal pages before super region
  if(pa_start < super_start)
    freerange((void*)pa_start, (void*)super_start);

  // 2) add super blocks
  for(uint64 p = super_start; p + SUPERPGSIZE <= super_end; p += SUPERPGSIZE){
    superfree((void*)p);
  }

  // 3) normal pages after super region
  if(super_end < pa_end)
    freerange((void*)super_end, (void*)pa_end);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

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

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}


void*
superalloc(void)
{
  acquire(&kmem.lock);
  struct run *r = kmem.superfreelist;
  if(r)
    kmem.superfreelist = r->next;
  release(&kmem.lock);
  // if(r == 0) printf("superalloc: empty\n");
  return (void*)r;  // 返回 2MB 起始地址
}

void
superfree(void *pa)
{
  if(((uint64)pa & SUPERPGMASK) != 0)
    panic("superfree: not aligned");

  acquire(&kmem.lock);
  struct run *r = (struct run*)pa;
  r->next = kmem.superfreelist;
  kmem.superfreelist = r;
  release(&kmem.lock);
}