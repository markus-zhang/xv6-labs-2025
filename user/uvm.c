#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

char buf[16];

__attribute__((optimize("O0")))
int main(int argc, char *argv[])
{
  for (int i = 0; i < 15; i++)
  {
    buf[i] = 'A' + i;
  }
  buf[15] = '\0';

  printf("Buffer is: %s\n", buf);
  exit(0);
}