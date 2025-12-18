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
#define HASH(dev, blockno) ((dev * 13 + blockno * 7) % NBUCKET)
struct bucket {
  struct spinlock lock;
  struct buf head;
};

struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  struct bucket buckets[NBUCKET];
} bcache;

void
binit(void)
{ 
  for(int i = 0; i < NBUCKET; i++) {
    char buf[16] = {};
    snprintf(buf, 16, "bcache_bucket%d", i);
    initlock(&bcache.buckets[i].lock, buf);
    bcache.buckets[i].head.next = &bcache.buckets[i].head;
    bcache.buckets[i].head.prev = &bcache.buckets[i].head;
  }

   for(struct buf *b = bcache.buf; b < bcache.buf+NBUF; b++){ //把所有buf初始化到buckets[0]的链表中
    initsleeplock(&b->lock, "buffer");
    b->next = bcache.buckets[0].head.next;
    b->prev = &bcache.buckets[0].head;
    bcache.buckets[0].head.next->prev = b;
    bcache.buckets[0].head.next = b;
  }

  initlock(&bcache.lock, "bcache");
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  int index = HASH(dev, blockno);
  struct bucket* bucket = &bcache.buckets[index];
  acquire(&bucket->lock);
  struct buf *head = &bucket->head;
  struct buf *b;
  int count = 0;

  // Is the block already cached?
  for(b = head->next; b != head; b = b->next){
    //printf("does this loop_1?\n");
    count++;
    if(count >= NBUF) {
      panic("bucket loop too long");
    }
    
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bucket->lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  struct bucket* another;
  for(another = bcache.buckets; another < bcache.buckets + NBUCKET; another++) {
    if(another != bucket) {
      acquire(&another->lock);
    }
    struct buf* another_head = &another->head;
    for(b = another_head->next; b != another_head; b = b->next) {
      if(b->refcnt == 0) {
        if(another != bucket) { //如果是同一个桶，不用重新插入
          //从旧链表中删除
          b->prev->next = b->next;
          b->next->prev = b->prev;
          //加入新链表
          b->prev = head;
          b->next = head->next;
          b->next->prev = b;
          head->next = b;
        }
        
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
        if(another != bucket) {
          release(&another->lock);
        }
        release(&bucket->lock);
        acquiresleep(&b->lock);
        return b;
      }
    }
    if(another != bucket) {
      release(&another->lock);
    }
  }
  panic("bget: no buffers");
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

  int index = HASH(b->dev, b->blockno);
  acquire(&bcache.buckets[index].lock);
  b->refcnt--;
  release(&bcache.buckets[index].lock);
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