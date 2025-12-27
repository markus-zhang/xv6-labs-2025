#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/debug.h"

char*
fmtname(char *path)
{
  static char buf[DIRSIZ+1];
  char *p;

  // Find first character after last slash.
  for(p=path+strlen(path); p >= path && *p != '/'; p--)
    ;
  p++;

  // Return blank-padded name.
  if(strlen(p) >= DIRSIZ)
    return p;
  memmove(buf, p, strlen(p));
  memset(buf+strlen(p), ' ', DIRSIZ-strlen(p));
  buf[sizeof(buf)-1] = '\0';
  return buf;
}

void
ls(char *path)
{
  char buf[512], *p;
  int fd;
  struct dirent de;
  struct stat st;

  //ANCHOR[id=ls_open]
  if((fd = open(path, O_RDONLY)) < 0){
    fprintf(2, "ls: cannot open %s\n", path);
    return;
  }

  if(fstat(fd, &st) < 0){
    fprintf(2, "ls: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch(st.type){
  case T_DEVICE:
  case T_FILE:
    printf("%s %d %d %d\n", fmtname(path), st.type, st.ino, (int) st.size);
    break;

  case T_SLINK:
    int dymlinksize = st.size;
    char target[DIRSIZ];
    read(fd, target, dymlinksize);
    printf("%s %d %d %d -> %s\n", fmtname(path), st.type, st.ino, (int) st.size, target);
    break;

  case T_DIR:
    if(strlen(path) + 1 + DIRSIZ + 1 > sizeof buf){
      printf("ls: path too long\n");
      break;
    }
    strcpy(buf, path);
    p = buf+strlen(buf);
    *p++ = '/';
    while(read(fd, &de, sizeof(de)) == sizeof(de)){
      if(de.inum == 0)
        continue;
      memmove(p, de.name, DIRSIZ);
      p[DIRSIZ] = 0;
      if(stat(buf, &st) < 0){
        printf("ls: cannot stat %s\n", buf);
        continue;
      }
      if (st.type == T_SLINK)
      {
        // int symlinkfd = open(buf, O_RDONLY);
        // struct stat symlinkst;
        // if(fstat(symlinkfd, &symlinkst) < 0)
        // {
        //   printf("ls: fstat failed\n");
        //   return;
        // }
        // int dymlinksize = symlinkst.size;
        // DPRINTF("ls: size of %s is %d\n", buf, dymlinksize);
        // char target[DIRSIZ];
        // read(symlinkfd, target, dymlinksize);
        int symlinkfd = open(buf, O_RDONLY);
        char target[DIRSIZ] = {0};
        symlinktarget(symlinkfd, (uint64)target);
        printf("%s %d %d %d -> %s\n", fmtname(buf), st.type, st.ino, (int) st.size, target);
      }
      else
        printf("%s %d %d %d\n", fmtname(buf), st.type, st.ino, (int) st.size);
    }
    break;
  }
  close(fd);
}

int
main(int argc, char *argv[])
{
  int i;

  if(argc < 2){
    ls(".");
    exit(0);
  }
  for(i=1; i<argc; i++)
    ls(argv[i]);
  exit(0);
}
