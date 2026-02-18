// On-disk file system format.
// Both the kernel and user programs use this header file.

#define ROOTINO 1  // root i-number
#define BSIZE 1024  // block size

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
 
// Maximum SUT blocks
#define NSUT_BLOCKS 8
#define LFS_NSEGS_MAX 1000

// Block types for SSB (must be non-zero, 0 means no SSB entry)
#define SSB_TYPE_DATA     1
#define SSB_TYPE_INODE    2
#define SSB_TYPE_INDIRECT 3

// Segment Summary Block Entry
struct ssb_entry {
  uchar type;    // Block type
  uint inum;     // Inode number (or start inum for INODE block)
  uint offset;   // Block offset within the file
  uint version;  // Inode version
};

// SSB Magic number for identification
#define SSB_MAGIC 0x53534221  // "SSB!"

// SSB entries per block: (BSIZE - 20) / 16 = 62 entries (adjusted for new header fields)
#define SSB_ENTRIES_PER_BLOCK ((BSIZE - 5*sizeof(uint)) / sizeof(struct ssb_entry))

// Segment Summary Block (on-disk format with header)
struct ssb {
  uint magic;         // SSB_MAGIC - SSB block identification
  uint nblocks;       // Number of data blocks this SSB describes
  uint checksum;      // Checksum of entries for integrity verification
  uint timestamp;     // Timestamp for roll-forward ordering
  uint next_seg_addr; // Next segment address (0 if not at segment boundary)
  struct ssb_entry entries[SSB_ENTRIES_PER_BLOCK];
};

// Segment Usage Table Entry
struct sut_entry {
  uint live_bytes;
  uint age;      // Last modification time (ticks or sequence)
};

// Checkpoint structure - stored at fixed location (exactly BSIZE bytes)
// Layout: [header_ts | metadata | padding | footer_ts]
// Header and footer timestamps must match for valid checkpoint
#define CP_METADATA_SIZE (6 * sizeof(uint) + NIMAP_BLOCKS * sizeof(uint) + NSUT_BLOCKS * sizeof(uint))
#define CP_PADDING_SIZE (BSIZE - CP_METADATA_SIZE - 2 * sizeof(uint))

struct checkpoint {
  // === Header (offset 0, first sector) ===
  uint timestamp;                    // Header timestamp - written FIRST

  // === Metadata ===
  uint log_tail;                     // Current log tail (next write position)
  uint cur_seg;                      // Current segment number
  uint seg_offset;                   // Offset within current segment
  uint imap_addrs[NIMAP_BLOCKS];     // Disk addresses of imap blocks
  uint imap_nblocks;                 // Number of imap blocks in use
  uint sut_addrs[NSUT_BLOCKS];       // Disk addresses of SUT blocks
  uint sut_nblocks;                  // Number of SUT blocks in use
  uint valid;                        // Is this checkpoint valid?

  // === Padding to push footer to end of block (last sector) ===
  uchar padding[CP_PADDING_SIZE];

  // === Footer (offset BSIZE-4, last 4 bytes) ===
  uint timestamp_end;                // Footer timestamp - written LAST
};

// Imap entries per block
#define IMAP_ENTRIES_PER_BLOCK (BSIZE / sizeof(uint))

// Imap entry encoding: block address + version + slot index
// imap[inum] = (block_addr << 12) | (version << 4) | slot_index
// slot_index: 0-15 (4 bits), version: 0-255 (8 bits), block_addr: remaining 20 bits
#define IMAP_SLOT_BITS 4
#define IMAP_VERSION_BITS 8
#define IMAP_SLOT_MASK ((1 << IMAP_SLOT_BITS) - 1)  // 0xF
#define IMAP_VERSION_MASK ((1 << IMAP_VERSION_BITS) - 1)  // 0xFF
#define IMAP_ENCODE(block, version, slot) \
  (((block) << (IMAP_VERSION_BITS + IMAP_SLOT_BITS)) | \
   (((version) & IMAP_VERSION_MASK) << IMAP_SLOT_BITS) | \
   ((slot) & IMAP_SLOT_MASK))
#define IMAP_BLOCK(entry) ((entry) >> (IMAP_VERSION_BITS + IMAP_SLOT_BITS))
#define IMAP_VERSION(entry) (((entry) >> IMAP_SLOT_BITS) & IMAP_VERSION_MASK)
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

