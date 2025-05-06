#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"

#define PGSIZE 4096

int
main(void)
{
  int fd = open("README", O_RDONLY);
  if (fd < 0) {
    printf("open(README) failed\n");
    exit(0);
  }

  char *p = (char*)mmap(0, PGSIZE,
                       PROT_READ,
                       MAP_POPULATE, fd, 0);
  if (!p) {
    printf("mmap failed\n");
    close(fd);
    exit(0);
  }

  // memmap: 직접 write()로 찍기
  printf("memmap: ");
  for (int i = 0; i < 20; i++) {
    char c = p[i];
    if (c == 0) c = '?';
    write(1, &c, 1);      // 1 = stdout
  }
  write(1, "\n", 1);

  // read()로 직접 읽어 비교
  char buf[21];
  int n = read(fd, buf, 20);
  if (n < 0) {
    printf("read() failed\n");
    munmap((uint64)p);
    close(fd);
    exit(0);
  }
  buf[n] = 0;
  printf("read() : %s\n", buf);

  munmap((uint64)p);
  close(fd);
  exit(0);
}