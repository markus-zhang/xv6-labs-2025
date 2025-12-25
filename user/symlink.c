#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  if(argc < 3){
    fprintf(2, "Usage: symlink <filename> <pathname>...\n");
    exit(1);
  }

  int nchar = strlen(argv[2]);
  if(symlink(argv[1], argv[2], nchar) < 0)
    fprintf(2, "symlink: %s %s failed to create symlink\n", argv[1], argv[2]);

  exit(0);
}
