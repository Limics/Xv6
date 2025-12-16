#include "kernel/types.h"
#include "user/user.h"

const int N = 280;

int p1[2];
int p2[2];

__attribute__((noreturn)) void seive() {
  // read the first number from left pipe and print
  int prime;
  close(p1[1]);
  if (read(p1[0], &prime, sizeof(prime))) {
    printf("prime %d\n", prime);
  } else {
    // EOF, left neighbor pipe only received one number
    // finised
    exit(0);
  }

  // read other numbers from left pipe
  int x;
  pipe(p2);
  // parent process: start to seive
  if (fork() > 0) {
    close(p2[0]);
    while (read(p1[0], &x, sizeof(x))) {
      if (x % prime != 0) {
        write(p2[1], &x, sizeof(x));
      }
    }
    close(p1[0]);
    close(p2[1]);
    wait(0);
    exit(0);
  }
  // child process: close unnecessary pipes and recur
  else {
    close(p1[0]);
    p1[0] = p2[0];
    p1[1] = p2[1];
    seive();
    exit(0);
  }
}

int main(int argc, char *argv[]) {
  pipe(p1);
  // parent process: write 2 ~ 280 to left pipe
  if (fork() > 0) {
    close(p1[0]);
    for (int i = 2; i <= N; i++) {
      write(p1[1], &i, sizeof(i));
    }
    close(p1[1]);
    wait(0);
    exit(0);
  }
  // child process: start to seive
  else {
    seive();
    close(p1[0]);
    exit(0);
  }
}