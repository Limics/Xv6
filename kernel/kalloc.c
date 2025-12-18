// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

#define STEAL_BATCH 256
void freerange(void *pa_start, void *pa_end);
static void steal(int me);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

static uchar page_owner[(PHYSTOP - KERNBASE) / PGSIZE];
static inline int
pageindex(void *pa)
{
  return ((uint64)pa - KERNBASE) / PGSIZE;
}

static void
set_owner_list(struct run *p, int owner)
{
  for(; p; p = p->next){
    page_owner[pageindex((void*)p)] = owner;
  }
}

void
kinit()
{
  for(int i = 0; i < NCPU; i++){
    initlock(&kmem[i].lock, "kmem");
    kmem[i].freelist = 0;
  }
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
  if((uint64)pa < KERNBASE)
    panic("kfree: pa < KERNBASE");

  memset(pa, 1, PGSIZE);
  r = (struct run*)pa;

  int idx = pageindex(pa);
  int id = page_owner[idx];
  if(id < 0 || id >= NCPU) id = 0;

  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);
}
// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r = 0;
  int id;

  push_off();
  id = cpuid();
  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if(r)
    kmem[id].freelist = r->next;
  release(&kmem[id].lock);
  pop_off();

  if(r == 0){
    steal(id);

    push_off();
    acquire(&kmem[id].lock);
    r = kmem[id].freelist;
    if(r)
      kmem[id].freelist = r->next;
    release(&kmem[id].lock);
    pop_off();
  }

  if(r){
    page_owner[pageindex((void*)r)] = id;
    memset((char*)r, 5, PGSIZE);
  }
  return (void*)r;
}

static void
steal(int me)
{
  for(int off = 1; off < NCPU; off++){
    int i = (me + off) % NCPU;

    acquire(&kmem[i].lock);
    struct run *head = kmem[i].freelist;
    if(head == 0){
      release(&kmem[i].lock);
      continue;
    }

    struct run *slow = head;
    struct run *fast = head;
    struct run *prev = 0;

    while(fast && fast->next){
      prev = slow;
      slow = slow->next;
      fast = fast->next->next;
    }

    struct run *stolen = head;
    struct run *remain = slow;

    if(prev){
      prev->next = 0;          // prev 是 stolen 的尾巴
      kmem[i].freelist = remain;
    } else {
      kmem[i].freelist = 0;    // 只有 1 个节点
      prev = stolen;           // 让 prev 也指向尾巴
    }
    release(&kmem[i].lock);

    set_owner_list(stolen, me);

    acquire(&kmem[me].lock);
    prev->next = kmem[me].freelist;  // O(1) 拼接
    kmem[me].freelist = stolen;
    release(&kmem[me].lock);
    return;
  }
}