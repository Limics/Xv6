#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

// xv6's ethernet and IP addresses
static uint8 local_mac[ETHADDR_LEN] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static uint32 local_ip = MAKE_IP_ADDR(10, 0, 2, 15);

// qemu host's ethernet address.
static uint8 host_mac[ETHADDR_LEN] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };

static struct spinlock netlock;

#define UDPQ_MAX 16
#define NPORTS   64   // 足够通过测试

struct udpqent {
  char   *pkt;       // 整个以太/IP/UDP/负载包的起始地址（kalloc 的）
  int     paylen;    // payload长度（host order）
  uint32  src_ip;    // host order
  uint16  src_port;  // host order
};

struct udpport {
  int used;
  uint16 port;       // host order
  struct udpqent q[UDPQ_MAX];
  int head, tail, count;
};

static struct udpport ports[NPORTS];

static struct udpport*
port_lookup(uint16 port)
{
  for(int i = 0; i < NPORTS; i++){
    if(ports[i].used && ports[i].port == port)
      return &ports[i];
  }
  return 0;
}

static struct udpport*
port_bind(uint16 port)
{
  struct udpport *p = port_lookup(port);
  if(p) return p;

  for(int i = 0; i < NPORTS; i++){
    if(ports[i].used == 0){
      ports[i].used = 1;
      ports[i].port = port;
      ports[i].head = ports[i].tail = ports[i].count = 0;
      return &ports[i];
    }
  }
  return 0;
}


void
netinit(void)
{
  initlock(&netlock, "netlock");
  for(int i = 0; i < NPORTS; i++)
    ports[i].used = 0;
}

//
// bind(int port)
// prepare to receive UDP packets address to the port,
// i.e. allocate any queues &c needed.
//
uint64
sys_bind(void)
{
  int port_i;
  argint(0, &port_i);

  if(port_i < 0 || port_i > 65535)
    return -1;

  uint16 port = (uint16)port_i;

  acquire(&netlock);
  struct udpport *p = port_bind(port);
  release(&netlock);

  return (p != 0) ? 0 : -1;
}

//
// unbind(int port)
// release any resources previously created by bind(port);
// from now on UDP packets addressed to port should be dropped.
//
uint64
sys_unbind(void)
{
  //
  // Optional: Your code here.
  //

  return 0;
}

//
// recv(int dport, int *src, short *sport, char *buf, int maxlen)
// if there's a received UDP packet already queued that was
// addressed to dport, then return it.
// otherwise wait for such a packet.
//
// sets *src to the IP source address.
// sets *sport to the UDP source port.
// copies up to maxlen bytes of UDP payload to buf.
// returns the number of bytes copied,
// and -1 if there was an error.
//
// dport, *src, and *sport are host byte order.
// bind(dport) must previously have been called.
//
uint64
sys_recv(void)
{
  int dport_i;
  uint64 src_u, sport_u, buf_u;
  int maxlen;

  argint(0, &dport_i);
  argaddr(1, &src_u);
  argaddr(2, &sport_u);
  argaddr(3, &buf_u);
  argint(4, &maxlen);

  if(dport_i < 0 || dport_i > 65535)
    return -1;
  if(maxlen < 0)
    return -1;

  uint16 dport = (uint16)dport_i;
  struct proc *pr = myproc();

  struct udpqent ent;

  acquire(&netlock);
  struct udpport *p = port_lookup(dport);
  if(p == 0){
    release(&netlock);
    return -1; // 没 bind
  }

  while(p->count == 0){
    if(killed(pr)){
      release(&netlock);
      return -1;
    }
    sleep(p, &netlock);
  }

  ent = p->q[p->head];
  p->head = (p->head + 1) % UDPQ_MAX;
  p->count--;

  release(&netlock);

  int ncopy = ent.paylen;
  if(ncopy > maxlen) ncopy = maxlen;

  char *payload = ent.pkt + sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);

  if(copyout(pr->pagetable, src_u, (char*)&ent.src_ip, sizeof(ent.src_ip)) < 0){
    kfree(ent.pkt);
    return -1;
  }
  if(copyout(pr->pagetable, sport_u, (char*)&ent.src_port, sizeof(ent.src_port)) < 0){
    kfree(ent.pkt);
    return -1;
  }
  if(ncopy > 0){
    if(copyout(pr->pagetable, buf_u, payload, ncopy) < 0){
      kfree(ent.pkt);
      return -1;
    }
  }

  kfree(ent.pkt);
  return ncopy;
}

// This code is lifted from FreeBSD's ping.c, and is copyright by the Regents
// of the University of California.
static unsigned short
in_cksum(const unsigned char *addr, int len)
{
  int nleft = len;
  const unsigned short *w = (const unsigned short *)addr;
  unsigned int sum = 0;
  unsigned short answer = 0;

  /*
   * Our algorithm is simple, using a 32 bit accumulator (sum), we add
   * sequential 16 bit words to it, and at the end, fold back all the
   * carry bits from the top 16 bits into the lower 16 bits.
   */
  while (nleft > 1)  {
    sum += *w++;
    nleft -= 2;
  }

  /* mop up an odd byte, if necessary */
  if (nleft == 1) {
    *(unsigned char *)(&answer) = *(const unsigned char *)w;
    sum += answer;
  }

  /* add back carry outs from top 16 bits to low 16 bits */
  sum = (sum & 0xffff) + (sum >> 16);
  sum += (sum >> 16);
  /* guaranteed now that the lower 16 bits of sum are correct */

  answer = ~sum; /* truncate to 16 bits */
  return answer;
}

//
// send(int sport, int dst, int dport, char *buf, int len)
//
uint64
sys_send(void)
{
  struct proc *p = myproc();
  int sport;
  int dst;
  int dport;
  uint64 bufaddr;
  int len;

  argint(0, &sport);
  argint(1, &dst);
  argint(2, &dport);
  argaddr(3, &bufaddr);
  argint(4, &len);

  int total = len + sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);
  if(total > PGSIZE)
    return -1;

  char *buf = kalloc();
  if(buf == 0){
    printf("sys_send: kalloc failed\n");
    return -1;
  }
  memset(buf, 0, PGSIZE);

  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, host_mac, ETHADDR_LEN);
  memmove(eth->shost, local_mac, ETHADDR_LEN);
  eth->type = htons(ETHTYPE_IP);

  struct ip *ip = (struct ip *)(eth + 1);
  ip->ip_vhl = 0x45; // version 4, header length 4*5
  ip->ip_tos = 0;
  ip->ip_len = htons(sizeof(struct ip) + sizeof(struct udp) + len);
  ip->ip_id = 0;
  ip->ip_off = 0;
  ip->ip_ttl = 100;
  ip->ip_p = IPPROTO_UDP;
  ip->ip_src = htonl(local_ip);
  ip->ip_dst = htonl(dst);
  ip->ip_sum = in_cksum((unsigned char *)ip, sizeof(*ip));

  struct udp *udp = (struct udp *)(ip + 1);
  udp->sport = htons(sport);
  udp->dport = htons(dport);
  udp->ulen = htons(len + sizeof(struct udp));

  char *payload = (char *)(udp + 1);
  if(copyin(p->pagetable, payload, bufaddr, len) < 0){
    kfree(buf);
    printf("send: copyin failed\n");
    return -1;
  }

  while(e1000_transmit(buf, total) < 0){
    // 让出 CPU，避免在内核里死占用
    yield();
  }
  return 0;
}

void
ip_rx(char *buf, int len)
{
  // don't delete this printf; make grade depends on it.
  static int seen_ip = 0;
  if(seen_ip == 0)
    printf("ip_rx: received an IP packet\n");
  seen_ip = 1;

  int need = sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);
  if(len < need){
    kfree(buf);
    return;
  }

  struct eth *eth = (struct eth *)buf;
  struct ip  *ip  = (struct ip *)(eth + 1);

  if(ip->ip_p != IPPROTO_UDP){
    kfree(buf);
    return;
  }

  struct udp *udp = (struct udp *)(ip + 1);

  uint16 dport = ntohs(udp->dport);
  uint16 sport = ntohs(udp->sport);
  uint16 ulen  = ntohs(udp->ulen);

  if(ulen < sizeof(struct udp)){
    kfree(buf);
    return;
  }

  int paylen = ulen - sizeof(struct udp);

  // 粗略确认包里确实包含这么多payload
  int have = len - (sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp));
  if(paylen > have){
    kfree(buf);
    return;
  }

  uint32 src_ip = ntohl(ip->ip_src);

  acquire(&netlock);

  struct udpport *p = port_lookup(dport);
  if(p == 0){
    release(&netlock);
    kfree(buf);      // 未 bind：丢弃
    return;
  }

  if(p->count >= UDPQ_MAX){
    release(&netlock);
    kfree(buf);      // 本端口队列满：丢弃
    return;
  }

  struct udpqent *e = &p->q[p->tail];
  e->pkt = buf;
  e->paylen = paylen;
  e->src_ip = src_ip;
  e->src_port = sport;

  p->tail = (p->tail + 1) % UDPQ_MAX;
  p->count++;

  wakeup(p);         // 唤醒等待 recv 的进程
  release(&netlock);

  return;            // buf 所有权交给队列，不能 kfree
  
}

//
// send an ARP reply packet to tell qemu to map
// xv6's ip address to its ethernet address.
// this is the bare minimum needed to persuade
// qemu to send IP packets to xv6; the real ARP
// protocol is more complex.
//
void
arp_rx(char *inbuf)
{
  static int seen_arp = 0;

  if(seen_arp){
    kfree(inbuf);
    return;
  }
  printf("arp_rx: received an ARP packet\n");
  seen_arp = 1;

  struct eth *ineth = (struct eth *) inbuf;
  struct arp *inarp = (struct arp *) (ineth + 1);

  char *buf = kalloc();
  if(buf == 0)
    panic("send_arp_reply");
  
  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, ineth->shost, ETHADDR_LEN); // ethernet destination = query source
  memmove(eth->shost, local_mac, ETHADDR_LEN); // ethernet source = xv6's ethernet address
  eth->type = htons(ETHTYPE_ARP);

  struct arp *arp = (struct arp *)(eth + 1);
  arp->hrd = htons(ARP_HRD_ETHER);
  arp->pro = htons(ETHTYPE_IP);
  arp->hln = ETHADDR_LEN;
  arp->pln = sizeof(uint32);
  arp->op = htons(ARP_OP_REPLY);

  memmove(arp->sha, local_mac, ETHADDR_LEN);
  arp->sip = htonl(local_ip);
  memmove(arp->tha, ineth->shost, ETHADDR_LEN);
  arp->tip = inarp->sip;

  while(e1000_transmit(buf, sizeof(*eth) + sizeof(*arp)) < 0) yield();
  kfree(inbuf);
}

void
net_rx(char *buf, int len)
{
  struct eth *eth = (struct eth *) buf;

  if(len >= sizeof(struct eth) + sizeof(struct arp) &&
     ntohs(eth->type) == ETHTYPE_ARP){
    arp_rx(buf);
  } else if(len >= sizeof(struct eth) + sizeof(struct ip) &&
     ntohs(eth->type) == ETHTYPE_IP){
    ip_rx(buf, len);
  } else {
    kfree(buf);
  }
}
