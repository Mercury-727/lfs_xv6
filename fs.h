// On-disk file system format.
// Both the kernel and user programs use this header file.

#define ROOTINO 1  // root i-number
#define BSIZE 512  // block size

// LFS Magic number
#define LFS_MAGIC 0x4C465321  // "LFS!"

// LFS Disk layout:
// [ boot block | super block | checkpoint0 | checkpoint1 | log (segments) ]

// Superblock describes the disk layout
struct superblock {
  uint magic;         // LFS magic number
  uint size;          // Size of file system image (blocks)
  uint nsegs;         // Number of segments
  uint segsize;       // Segment size (blocks)
  uint segstart;      // Block number of first segment
  uint ninodes;       // Maximum number of inodes
  uint checkpoint0;   // Block number of checkpoint 0
  uint checkpoint1;   // Block number of checkpoint 1
};

// Maximum imap blocks (each block holds 128 inode locations)
#define NIMAP_BLOCKS 4

// Checkpoint structure - stored at fixed location
struct checkpoint {
  uint timestamp;                    // Checkpoint timestamp
  uint log_tail;                     // Current log tail (next write position)
  uint cur_seg;                      // Current segment number
  uint seg_offset;                   // Offset within current segment
  uint imap_addrs[NIMAP_BLOCKS];     // Disk addresses of imap blocks
  uint imap_nblocks;                 // Number of imap blocks in use
  uint valid;                        // Is this checkpoint valid?
};

// Imap entries per block
#define IMAP_ENTRIES_PER_BLOCK (BSIZE / sizeof(uint))

// Imap entry encoding: block address + slot index
// imap[inum] = (block_addr << 3) | slot_index
// slot_index: 0-7 (3 bits), block_addr: remaining bits
#define IMAP_SLOT_BITS 3
#define IMAP_SLOT_MASK ((1 << IMAP_SLOT_BITS) - 1)  // 0x7
#define IMAP_ENCODE(block, slot) (((block) << IMAP_SLOT_BITS) | ((slot) & IMAP_SLOT_MASK))
#define IMAP_BLOCK(entry) ((entry) >> IMAP_SLOT_BITS)
#define IMAP_SLOT(entry) ((entry) & IMAP_SLOT_MASK)

#define NDIRECT 12
#define NINDIRECT (BSIZE / sizeof(uint))
#define MAXFILE (NDIRECT + NINDIRECT)

// On-disk inode structure (unchanged from original xv6)
struct dinode {
  short type;           // File type
  short major;          // Major device number (T_DEV only)
  short minor;          // Minor device number (T_DEV only)
  short nlink;          // Number of links to inode in file system
  uint size;            // Size of file (bytes)
  uint addrs[NDIRECT+1];   // Data block addresses
};

// Inodes per block
#define IPB           (BSIZE / sizeof(struct dinode))

// Bitmap bits per block (not used in LFS, kept for compatibility)
#define BPB           (BSIZE*8)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

struct dirent {
  ushort inum;
  char name[DIRSIZ];
};

