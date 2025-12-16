#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

#include "kernel/param.h"

#define PGSIZE 4096

static int
okchar(char c) {
  return (c >= 'a' && c <= 'z') || c == '.' || c == '/';
}

int
main(int argc, char *argv[]) {
  // 1) 把 brk 对齐到页边界，避免拿到“半页”
  char *brk = sbrk(0);
  uint64 mis = (uint64)brk % PGSIZE;
  if(mis)
    sbrk(PGSIZE - mis);

  // 2) 申请多页，逐页检查 offset 32 的 8 字节
  for(int i = 0; i < 200; i++){
    char *p = sbrk(PGSIZE);
    if(p == (char*)-1)
      exit(1);

    char *q = p + 32;

    // 3) 用一个字符集过滤
    int good = 1;
    for(int j = 0; j < 7; j++){
      if(!okchar(q[j])){
        good = 0;
        break;
      }
    }
    if(good && q[7] == 0){
      write(2, q, 8);   // 8 字节：7 chars + '\0'
      exit(0);
    }
  }
  // 没找到就失败退出
  exit(1);
}