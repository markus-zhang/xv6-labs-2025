#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

__attribute__((optimize("O0")))
int main(int argc, char *argv[])
{
  fsdump();

  return 0;
}