//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "memlayout.h"
#define MMAPTOP (TRAPFRAME)

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;
  
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0){
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if(dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if(type == T_DIR){
    // now that success is guaranteed:
    dp->nlink++;  // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

 fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if((argstr(0, path, MAXPATH)) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

static struct vma*
vma_alloc(struct proc *p)
{
  for(int i=0;i<NVMA;i++){
    if(p->vmas[i].used == 0){
      p->vmas[i].used = 1;
      return &p->vmas[i];
    }
  }
  return 0;
}

static uint64
vma_find_space(struct proc *p, uint64 len)
{
  uint64 base = MMAPTOP;
  uint64 cand = PGROUNDDOWN(base - len);

  for(int tries=0; tries<NVMA+2; tries++){
    int overlap = 0;
    for(int i=0;i<NVMA;i++){
      if(!p->vmas[i].used) continue;
      uint64 a0=p->vmas[i].addr, a1=p->vmas[i].addr+p->vmas[i].len;
      uint64 b0=cand, b1=cand+len;
      if(!(b1<=a0 || b0>=a1)){
        overlap = 1;
        cand = PGROUNDDOWN(a0 - len); // 放到这个 VMA 下方再试
        break;
      }
    }
    if(!overlap){
      if(cand < p->sz) return 0; 
      return cand;
    }
  }
  return 0;
}

uint64
sys_mmap(void)
{
  uint64 uaddr;
  int len, prot, flags, fd;
  int offset;

  argaddr(0, &uaddr);
  argint(1, &len);
  argint(2, &prot);
  argint(3, &flags);
  argint(4, &fd);
  argint(5, &offset);

  if(uaddr != 0) return (uint64)-1;
  if(len <= 0) return (uint64)-1;
  if(offset != 0) return (uint64)-1;
  if(flags != MAP_SHARED && flags != MAP_PRIVATE) return (uint64)-1;
  if((prot & (PROT_READ|PROT_WRITE)) == 0) return (uint64)-1;

  if(fd < 0 || fd >= NOFILE) return (uint64)-1;

  struct proc *p = myproc();
  struct file *f = p->ofile[fd];
  if(f == 0) return (uint64)-1;
  if(f->type != FD_INODE) return (uint64)-1;

  if((prot & PROT_READ) && f->readable == 0)
    return (uint64)-1;
  if((prot & PROT_WRITE) && flags == MAP_SHARED && f->writable == 0)
    return (uint64)-1;

  uint64 mlen = PGROUNDUP((uint64)len);
  uint64 va = vma_find_space(p, mlen);
  if(va == 0) return (uint64)-1;

  struct vma *v = vma_alloc(p);
  if(v == 0) return (uint64)-1;

  v->addr = va;
  v->len = mlen;
  v->prot = prot;
  v->flags = flags;
  v->foff = 0;
  v->f = filedup(f);

  return va;
}


static int
vma_writeback(struct vma *v, uint64 va, uint64 kva)  // kva: 页的内核地址
{
  uint64 pageoff = va - v->addr;
  // uint64 remain  = v->len - pageoff;
  uint64 endva = v->addr + v->len;
  uint64 page_end = va + PGSIZE;
  int n;
  if(page_end <= endva)
    n = PGSIZE;
  else if(va < endva)
    n = endva - va;
  else
    return 0;
  begin_op();
  ilock(v->f->ip);

  // writei 会在需要时更新 ip->size，所以你自己改 size 其实不必
  int r = writei(v->f->ip, 0, kva, v->foff + pageoff, n);

  iunlock(v->f->ip);
  end_op();

  if(r != n) return -1;   // ✅ 关键：必须写满
  return 0;
}

static struct vma*
vma_lookup(struct proc *p, uint64 va)
{
  for(int i = 0; i < NVMA; i++){
    if(!p->vmas[i].used) continue;
    if(va >= p->vmas[i].addr && va < p->vmas[i].addr + p->vmas[i].len)
      return &p->vmas[i];
  }
  return 0;
}

static inline uint64
pa2kva(uint64 pa)
{
  // 如果 pa 已经在内核直映区（>=KERNBASE），直接返回
  // 否则加上 KERNBASE 把物理地址变成内核虚拟地址
  if(pa >= KERNBASE)
    return pa;
  return pa + KERNBASE;
}

int
do_munmap(struct proc *p, uint64 addr, uint64 len)
{
  if(len <= 0) return 0;
  uint64 a0 = PGROUNDDOWN(addr);
  uint64 a1 = PGROUNDUP(addr + len);

  struct vma *v = vma_lookup(p, a0);
  if(v == 0) return 0; // mmaptest 期望“没映射也 OK”

  // 限制：只能从头/尾/全段
  uint64 v0=v->addr, v1=v->addr+v->len;
  if(!(a0==v0 || a1==v1 || (a0==v0 && a1==v1))) return -1;
  if(a0 < v0 || a1 > v1) return -1;

  for(uint64 va=a0; va<a1; va+=PGSIZE){
    pte_t *pte = walk(p->pagetable, va, 0);
    if(pte && (*pte & PTE_V)){
      uint64 pa = PTE2PA(*pte);
      uint64 kva = pa;

      if(kva < KERNBASE)
        kva = pa + KERNBASE;

      if(v->flags == MAP_SHARED){
        if(vma_writeback(v, va, kva) < 0)
          return -1;
      }
      uvmunmap(p->pagetable, va, 1, 1);
    }
  }

  // 更新 VMA
  uint64 unlen = a1 - a0;
  if(a0 == v->addr && a1 == v->addr + v->len){
    // 全删
    struct file *f = v->f;
    v->used = 0;
    v->f = 0;
    fileclose(f);
  } else if(a0 == v->addr){
    // 删头
    v->addr += unlen;
    v->len  -= unlen;
    v->foff += unlen;
  } else { // a1 == v1
    // 删尾
    v->len -= unlen;
  }
  return 0;
}

uint64
sys_munmap(void)
{
  uint64 addr;
  int len;
  argaddr(0, &addr);
  argint(1, &len);

  struct proc *p = myproc();
  return do_munmap(p, addr, (uint64)len);
}