#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char* argv[]) {
  char buf[] = {'o'};

  int p1[2], p2[2];
  pipe(p1);  // parent -> child pipe
  pipe(p2);  // child -> parent pipe

  if (fork() == 0) {
    close(p1[1]);
    close(p2[0]);
    if (read(p1[0], buf, sizeof buf)) {
      printf("%d: received ping\n", getpid());
    }
    close(p1[0]);
    write(p2[1], buf, sizeof(buf));
    close(p2[1]);
    exit(0);
  } else {
    close(p1[0]);
    close(p2[1]);
    write(p1[1], buf, sizeof(buf));
    close(p1[1]);
    wait(0);
    if (read(p2[1], buf, sizeof(buf))) {
      printf("%d: received pong\n", getpid());
    }
    close(p2[0]);
    exit(0);
  }
}