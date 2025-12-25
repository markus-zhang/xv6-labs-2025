// On-disk file system format.
// Both the kernel and user programs use this header file.


#define ROOTINO  1   // root i-number
#define BSIZE 1024  // block size

// Disk layout:
// [ boot block | super block | log | inode blocks |
//                                          free bit map | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
  uint magic;        // Must be FSMAGIC
  uint size;         // Size of file system image (blocks)
  uint nblocks;      // Number of data blocks
  uint ninodes;      // Number of inodes.
  uint nlog;         // Number of log blocks
  uint logstart;     // Block number of first log block
  uint inodestart;   // Block number of first inode block
  uint bmapstart;    // Block number of first free map block
};

#define FSMAGIC 0x10203040

// #define NDIRECT 12
#define NDIRECT 11
//NOTE - NINDIRECT = 1024 / 4 = 256 in this system
#define NINDIRECT (BSIZE / sizeof(uint))
// #define MAXFILE (NDIRECT + NINDIRECT)
#define MAXFILE (NDIRECT + NINDIRECT + NINDIRECT * NINDIRECT)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

// On-disk inode structure
struct dinode {
  short type;           // File type
  short major;          // Major device number (T_DEVICE only)
  short minor;          // Minor device number (T_DEVICE only)
  short nlink;          // Number of links to inode in file system
  uint size;            // Size of file (bytes)
  //NOTE - The last entry of addrs gives the address of the INDIRECT BLOCK
  // uint addrs[NDIRECT+1];   // Data block addresses
  //NOTE - Big File Lab: 11 NIDIRECT + 1 single-indirect + 1 double-indirect
  //single-indirect = original design (pointing to a 256-uint blockn block)
  //double-indirect = pointing to a 256-uint indirect block, 
  //each uint pointing to a 256-uint blockn block
  uint addrs[NDIRECT+2];   // Data block addresses

  //NOTE: Symbolic Link Lab
  uint symlkinum;
};

// Inodes per block.
#define IPB           (BSIZE / sizeof(struct dinode))

// Block containing inode i
#define IBLOCK(i, sb)     ((i) / IPB + sb.inodestart)

// Bitmap bits per block
#define BPB           (BSIZE*8)

// Block of free map containing bit for block b
#define BBLOCK(b, sb) ((b)/BPB + sb.bmapstart)

// The name field may have DIRSIZ characters and not end in a NUL
// character.
struct dirent {
  //NOTE - inum is the inode number
  ushort inum;
  char name[DIRSIZ] __attribute__((nonstring));
};

