// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end, int cpu_index);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

static char digits[] = "0123456789abcdef";

struct {
  struct spinlock lock;
  struct run *freelist;
  //uint64 pg_num;   //拥有物理页的数量，用于寻找拥有物理页最多的CPU
} kmem[NCPU];

void
kinit()
{
  char name[] = "kmem_CPU ";
  uint64 per_cpu_range = (PHYSTOP - (uint64)end) / NCPU;
  for(int i = 0; i < NCPU; i++) {
    name[8] = digits[i];
    initlock(&kmem[i].lock, name);
    void* pa_start = end + i * per_cpu_range;
    void* pa_end = pa_start + per_cpu_range;
    freerange(pa_start, pa_end, i);
  }
}

void
freerange(void *pa_start, void *pa_end, int cpu_index)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  struct run *r;
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE) {
    memset(p, 1, PGSIZE);
    r = (struct run*)p;
    acquire(&kmem[cpu_index].lock);
    r->next = kmem[cpu_index].freelist;
    kmem[cpu_index].freelist = r;
    release(&kmem[cpu_index].lock);
  }
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
  push_off();
  int cpu_index = cpuid();
  pop_off();

  acquire(&kmem[cpu_index].lock);
  r->next = kmem[cpu_index].freelist;
  kmem[cpu_index].freelist = r;
  release(&kmem[cpu_index].lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  push_off();
  int cpu_index = cpuid();
  pop_off();

  acquire(&kmem[cpu_index].lock);
  r = kmem[cpu_index].freelist;
  if(r) {
    kmem[cpu_index].freelist = r->next;
    release(&kmem[cpu_index].lock);
  }
  else {
    release(&kmem[cpu_index].lock); //释放锁，我的写法不放应该也不会有死锁
    for(int i = 0; i < NCPU; i++) {
      if(i == cpu_index) continue;
      if(kmem[i].lock.locked && kmem[i].freelist != 0) continue; //此处有风险，但应该不会有并发或并行问题
      acquire(&kmem[i].lock);   //小概率因为并行在此处陷入等待
      if(!kmem[i].freelist) { //二次确认，防止因并行导致cpu i拥有的物理页变为0
        release(&kmem[i].lock);
        continue;
      }
      r = kmem[i].freelist;
      kmem[i].freelist = r->next;
      release(&kmem[i].lock);
      break;
    }
  }

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}