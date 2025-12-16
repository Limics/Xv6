#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(2, "usage: xargs <command>\n");
    exit(1);
  }

  // declare an new argv array
  char *new_argv[MAXARG], **arg_ptr = new_argv;
  char buf[512];  // store extra arguments read from standard input
  char *p = buf;  // position of current char
  char *r = buf;  // position of current argument

  // get original arguments
  for (int i = 1; i < argc; i++) {
    *arg_ptr++ = argv[i];
  }

  // get extra arguments
  while (read(0, p, 1) == 1) {
    if (*p == ' ') {
      *p = 0;
      *arg_ptr++ = r;  // add argment to new_argv
      r = ++p;         // update pointers
    } else if (*p == '\n') {
      *p = 0;
      *arg_ptr = r;
      if (fork() == 0) {
        exec(argv[1], new_argv);
        exit(0);
      }
      wait(0);
      p = r = buf;  // reset pointers
    } else {
      p++;
    }
  }
  exit(0);
}