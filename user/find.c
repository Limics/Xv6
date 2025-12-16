#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

/// @brief format path2file like "/usr/local/bin/clash" to filename "clash"
/// @param path2file
/// @return filename
char *fmtname(char *path) {
  static char buf[DIRSIZ + 1];
  char *p;

  // find first character after last slash.
  for (p = path + strlen(path); p >= path && *p != '/'; p--);
  p++;

  // keep only the file name, remove the path.
  memmove(buf, p, strlen(p));
  buf[strlen(p)] = 0;
  return buf;
}

void find(char *path, char *filename) {
  char buf[512], *p;
  int fd;
  struct dirent de;
  struct stat st;

  if ((fd = open(path, 0)) < 0) {
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }

  if (fstat(fd, &st) < 0) {
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch (st.type) {
    case T_FILE:
      if (strcmp(fmtname(path), filename) == 0) {
        printf("%s\n", path);
      }
      break;
    case T_DIR:
      if (strlen(path) + 1 + DIRSIZ + 1 > sizeof buf) {
        fprintf(2, "find: path too long\n");
        return;
      }
      strcpy(buf, path);
      p = buf + strlen(buf);
      *p++ = '/';
      while (read(fd, &de, sizeof(de)) == sizeof(de)) {
        if (de.inum == 0) continue;
        memmove(p, de.name, DIRSIZ);
        p[DIRSIZ] = 0;
        if (strcmp(filename, de.name) == 0) {
          printf("%s\n", buf);
          continue;
        }
        if (stat(buf, &st) < 0) {
          printf("ls: cannot stat %s\n", buf);
          continue;
        }
        // don't recurse into "." and ".."
        if (strcmp(de.name, ".") != 0 && strcmp(de.name, "..")) {
          find(buf, filename);
        }
      }
      break;
  }  // switch
  close(fd);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(2, "usage: find <path> <filename>\n");
    exit(1);
  }

  if (argc == 2) {
    find(".", argv[1]);
  }

  for (int i = 2; i < argc; i++) {
    find(argv[1], argv[i]);
  }
  exit(0);
}

/*
echo > b
mkdir a
echo > a/b
mkdir a/aa
echo > a/aa/b
find . b
*/