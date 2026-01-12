#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#define stat xv6_stat  // avoid clash with host struct stat
#include "types.h"
#include "fs.h"
#include "stat.h"
#include "param.h"

#ifndef static_assert
#define static_assert(a, b) do { switch (0) case 0: case (a): ; } while (0)
#endif

// LFS Disk layout:
// [ boot block | sb block | checkpoint0 | checkpoint1 | log (segments) ]

int fsfd;
struct superblock sb;
struct checkpoint cp;
char zeroes[BSIZE];

// In-memory imap (uses IMAP_ENCODE format: block << 3 | slot)
uint imap[LFS_NINODES];
uint freeinode = 1;  // next free inode number (0 is reserved)
uint log_tail;       // current position in log

// Dirty inode buffer for batching (8 inodes per block)
struct dinode dirty_inodes[IPB];
uint dirty_inums[IPB];
int dirty_count = 0;

void wsect(uint, void*);
void rsect(uint sec, void *buf);
uint lfs_alloc(void);
void lfs_flush_inodes(void);
void lfs_write_inode(uint inum, struct dinode *dip);
void lfs_write_imap(void);
void lfs_write_checkpoint(void);
uint ialloc(ushort type);
void iappend(uint inum, void *p, int n);

// convert to intel byte order
ushort
xshort(ushort x)
{
  ushort y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  return y;
}

uint
xint(uint x)
{
  uint y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  a[2] = x >> 16;
  a[3] = x >> 24;
  return y;
}

int
main(int argc, char *argv[])
{
  int i, cc, fd;
  uint rootino, inum, off;
  struct dirent de;
  char buf[BSIZE];

  static_assert(sizeof(int) == 4, "Integers must be 4 bytes!");

  if(argc < 2){
    fprintf(stderr, "Usage: mkfs fs.img files...\n");
    exit(1);
  }

  assert((BSIZE % sizeof(struct dinode)) == 0);
  assert((BSIZE % sizeof(struct dirent)) == 0);

  fsfd = open(argv[1], O_RDWR|O_CREAT|O_TRUNC, 0666);
  if(fsfd < 0){
    perror(argv[1]);
    exit(1);
  }

  // Initialize superblock
  sb.magic = xint(LFS_MAGIC);
  sb.size = xint(FSSIZE);
  sb.nsegs = xint((FSSIZE - LFS_SEGSTART) / LFS_SEGSIZE);
  sb.segsize = xint(LFS_SEGSIZE);
  sb.segstart = xint(LFS_SEGSTART);
  sb.ninodes = xint(LFS_NINODES);
  sb.checkpoint0 = xint(2);  // block 2
  sb.checkpoint1 = xint(3);  // block 3

  printf("LFS: size %d, nsegs %d, segsize %d, segstart %d, ninodes %d\n",
         FSSIZE, (FSSIZE - LFS_SEGSTART) / LFS_SEGSIZE, LFS_SEGSIZE,
         LFS_SEGSTART, LFS_NINODES);

  // Initialize log tail to start of log area
  log_tail = LFS_SEGSTART;

  // Initialize imap (all zeros = no inodes allocated)
  memset(imap, 0, sizeof(imap));

  // Zero out entire disk
  for(i = 0; i < FSSIZE; i++)
    wsect(i, zeroes);

  // Write superblock
  memset(buf, 0, sizeof(buf));
  memmove(buf, &sb, sizeof(sb));
  wsect(1, buf);

  // Create root directory
  rootino = ialloc(T_DIR);
  assert(rootino == ROOTINO);

  // Add "." entry
  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, ".");
  iappend(rootino, &de, sizeof(de));

  // Add ".." entry
  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, "..");
  iappend(rootino, &de, sizeof(de));

  // Add files from command line
  for(i = 2; i < argc; i++){
    assert(index(argv[i], '/') == 0);

    if((fd = open(argv[i], 0)) < 0){
      perror(argv[i]);
      exit(1);
    }

    // Skip leading _ in name when writing to file system.
    if(argv[i][0] == '_')
      ++argv[i];

    inum = ialloc(T_FILE);

    bzero(&de, sizeof(de));
    de.inum = xshort(inum);
    strncpy(de.name, argv[i], DIRSIZ);
    iappend(rootino, &de, sizeof(de));

    while((cc = read(fd, buf, sizeof(buf))) > 0)
      iappend(inum, buf, cc);

    close(fd);
  }

  // Fix size of root inode dir to be block aligned
  {
    struct dinode din;
    int i, found = 0;

    // Check dirty buffer first
    for(i = 0; i < dirty_count; i++){
      if(dirty_inums[i] == rootino){
        memmove(&din, &dirty_inodes[i], sizeof(din));
        found = 1;
        break;
      }
    }

    if(!found){
      // Read from disk using IMAP_BLOCK/IMAP_SLOT
      char ibuf[BSIZE];
      struct dinode *dip;
      uint block = IMAP_BLOCK(imap[rootino]);
      uint slot = IMAP_SLOT(imap[rootino]);

      rsect(block, ibuf);
      dip = (struct dinode*)ibuf + slot;
      memmove(&din, dip, sizeof(din));
    }

    off = xint(din.size);
    off = ((off/BSIZE) + 1) * BSIZE;
    din.size = xint(off);

    // Write back via lfs_write_inode (handles buffer)
    lfs_write_inode(rootino, &din);
  }

  // Write final checkpoint (includes flush and imap write)
  lfs_write_checkpoint();

  printf("LFS: log_tail at block %d\n", log_tail);

  exit(0);
}

void
wsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE){
    perror("lseek");
    exit(1);
  }
  if(write(fsfd, buf, BSIZE) != BSIZE){
    perror("write");
    exit(1);
  }
}

void
rsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE){
    perror("lseek");
    exit(1);
  }
  if(read(fsfd, buf, BSIZE) != BSIZE){
    perror("read");
    exit(1);
  }
}

// Allocate a block from the log tail
uint
lfs_alloc(void)
{
  uint block = log_tail;
  log_tail++;
  if(log_tail >= FSSIZE){
    fprintf(stderr, "lfs_alloc: out of disk space\n");
    exit(1);
  }
  return block;
}

// Flush dirty inodes to a single block
void
lfs_flush_inodes(void)
{
  char buf[BSIZE];
  struct dinode *dip;
  uint block;
  int i;

  if(dirty_count == 0)
    return;

  block = lfs_alloc();

  memset(buf, 0, BSIZE);
  dip = (struct dinode*)buf;
  for(i = 0; i < dirty_count; i++){
    memmove(&dip[i], &dirty_inodes[i], sizeof(struct dinode));
    // Update imap with encoded value (block << 3 | slot)
    imap[dirty_inums[i]] = IMAP_ENCODE(block, i);
  }
  wsect(block, buf);

  dirty_count = 0;
}

// Write inode to the dirty buffer (batched write)
void
lfs_write_inode(uint inum, struct dinode *dip)
{
  int i;

  // Check if this inode is already in the buffer
  for(i = 0; i < dirty_count; i++){
    if(dirty_inums[i] == inum){
      memmove(&dirty_inodes[i], dip, sizeof(*dip));
      return;
    }
  }

  // If buffer is full, flush first
  if(dirty_count >= IPB){
    lfs_flush_inodes();
  }

  // Add to buffer
  memmove(&dirty_inodes[dirty_count], dip, sizeof(*dip));
  dirty_inums[dirty_count] = inum;
  dirty_count++;
}

// Write imap blocks to the log
void
lfs_write_imap(void)
{
  char buf[BSIZE];
  uint nblocks = (LFS_NINODES + IMAP_ENTRIES_PER_BLOCK - 1) / IMAP_ENTRIES_PER_BLOCK;
  uint i, j;

  cp.imap_nblocks = xint(nblocks);

  for(i = 0; i < nblocks && i < NIMAP_BLOCKS; i++){
    uint block = lfs_alloc();
    cp.imap_addrs[i] = xint(block);

    memset(buf, 0, BSIZE);
    uint *p = (uint*)buf;
    for(j = 0; j < IMAP_ENTRIES_PER_BLOCK && (i * IMAP_ENTRIES_PER_BLOCK + j) < LFS_NINODES; j++){
      p[j] = xint(imap[i * IMAP_ENTRIES_PER_BLOCK + j]);
    }
    wsect(block, buf);
  }
}

// Write checkpoint to fixed location
void
lfs_write_checkpoint(void)
{
  char buf[BSIZE];

  // Flush any pending dirty inodes first
  lfs_flush_inodes();

  // Write imap (after flush, so addresses are correct)
  lfs_write_imap();

  cp.timestamp = xint(1);  // simple timestamp
  cp.log_tail = xint(log_tail);
  cp.cur_seg = xint((log_tail - LFS_SEGSTART) / LFS_SEGSIZE);
  cp.seg_offset = xint((log_tail - LFS_SEGSTART) % LFS_SEGSIZE);
  cp.valid = xint(1);

  memset(buf, 0, BSIZE);
  memmove(buf, &cp, sizeof(cp));

  // Write to checkpoint0
  wsect(2, buf);

  printf("Checkpoint written: log_tail=%d, imap_nblocks=%d\n",
         log_tail, xint(cp.imap_nblocks));
}

// Allocate a new inode
uint
ialloc(ushort type)
{
  uint inum = freeinode++;
  struct dinode din;

  if(inum >= LFS_NINODES){
    fprintf(stderr, "ialloc: no inodes\n");
    exit(1);
  }

  bzero(&din, sizeof(din));
  din.type = xshort(type);
  din.nlink = xshort(1);
  din.size = xint(0);

  lfs_write_inode(inum, &din);

  return inum;
}

#define min(a, b) ((a) < (b) ? (a) : (b))

// Append data to an inode
void
iappend(uint inum, void *xp, int n)
{
  char *p = (char*)xp;
  uint fbn, off, n1;
  struct dinode din;
  char buf[BSIZE];
  char ibuf[BSIZE];
  uint indirect[NINDIRECT];
  uint x;
  int i, found = 0;

  // Read current inode - first check dirty buffer
  for(i = 0; i < dirty_count; i++){
    if(dirty_inums[i] == inum){
      memmove(&din, &dirty_inodes[i], sizeof(din));
      found = 1;
      break;
    }
  }

  if(!found){
    // Read from disk using IMAP_BLOCK/IMAP_SLOT
    uint block = IMAP_BLOCK(imap[inum]);
    uint slot = IMAP_SLOT(imap[inum]);
    struct dinode *dip;

    rsect(block, ibuf);
    dip = (struct dinode*)ibuf + slot;
    memmove(&din, dip, sizeof(din));
  }

  off = xint(din.size);

  while(n > 0){
    fbn = off / BSIZE;
    assert(fbn < MAXFILE);

    if(fbn < NDIRECT){
      if(xint(din.addrs[fbn]) == 0){
        din.addrs[fbn] = xint(lfs_alloc());
      }
      x = xint(din.addrs[fbn]);
    } else {
      if(xint(din.addrs[NDIRECT]) == 0){
        din.addrs[NDIRECT] = xint(lfs_alloc());
        // Zero out indirect block
        memset(indirect, 0, sizeof(indirect));
        wsect(xint(din.addrs[NDIRECT]), indirect);
      }
      rsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      if(indirect[fbn - NDIRECT] == 0){
        indirect[fbn - NDIRECT] = xint(lfs_alloc());
        wsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      }
      x = xint(indirect[fbn-NDIRECT]);
    }

    n1 = min(n, (fbn + 1) * BSIZE - off);
    rsect(x, buf);
    bcopy(p, buf + off - (fbn * BSIZE), n1);
    wsect(x, buf);
    n -= n1;
    off += n1;
    p += n1;
  }

  din.size = xint(off);

  // Write updated inode back to a new location in log
  lfs_write_inode(inum, &din);
}
