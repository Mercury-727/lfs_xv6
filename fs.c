// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.
//
// LFS (Log-structured File System) implementation:
// - All writes go to the log tail
// - Inode locations tracked via imap
// - Checkpoint stores imap locations

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))

static void itrunc(struct inode*);
static void lfs_flush_inodes(void);
static void lfs_write_imap(void);

// There should be one superblock per disk device, but we run with only one device
struct superblock sb;

// LFS state
struct {
  struct spinlock lock;
  uint imap[LFS_NINODES];      // inode number -> (block_addr << 3) | slot_index
  struct checkpoint cp;        // current checkpoint
  uint log_tail;               // next block to write
  int dev;                     // device number
} lfs;

// Dirty inode buffer - holds up to IPB (8) inodes before writing
struct {
  struct spinlock lock;
  struct dinode inodes[IPB];   // buffered dirty inodes
  uint inums[IPB];             // corresponding inode numbers
  int count;                   // number of dirty inodes in buffer
} dirty_inodes;

// Read the super block.
void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Read checkpoint from disk
static void
lfs_read_checkpoint(int dev)
{
  struct buf *bp;

  // Read checkpoint0
  bp = bread(dev, sb.checkpoint0);
  memmove(&lfs.cp, bp->data, sizeof(lfs.cp));
  brelse(bp);

  if(lfs.cp.valid == 0){
    panic("lfs_read_checkpoint: invalid checkpoint");
  }

  lfs.log_tail = lfs.cp.log_tail;
}

// Read imap from disk (locations stored in checkpoint)
static void
lfs_read_imap(int dev)
{
  struct buf *bp;
  uint i, j;
  uint *p;

  memset(lfs.imap, 0, sizeof(lfs.imap));

  for(i = 0; i < lfs.cp.imap_nblocks && i < NIMAP_BLOCKS; i++){
    bp = bread(dev, lfs.cp.imap_addrs[i]);
    p = (uint*)bp->data;
    for(j = 0; j < IMAP_ENTRIES_PER_BLOCK && (i * IMAP_ENTRIES_PER_BLOCK + j) < LFS_NINODES; j++){
      lfs.imap[i * IMAP_ENTRIES_PER_BLOCK + j] = p[j];
    }
    brelse(bp);
  }
}

// Write checkpoint to disk (just checkpoint, no flush/imap)
static void
lfs_write_checkpoint(void)
{
  struct buf *bp;
  struct checkpoint cp_copy;

  // Copy checkpoint data under lock
  acquire(&lfs.lock);
  lfs.cp.timestamp++;
  lfs.cp.log_tail = lfs.log_tail;
  lfs.cp.cur_seg = (lfs.log_tail - sb.segstart) / sb.segsize;
  lfs.cp.seg_offset = (lfs.log_tail - sb.segstart) % sb.segsize;
  lfs.cp.valid = 1;
  memmove(&cp_copy, &lfs.cp, sizeof(cp_copy));
  release(&lfs.lock);

  // Write checkpoint (outside lock)
  bp = bread(lfs.dev, sb.checkpoint0);
  memmove(bp->data, &cp_copy, sizeof(cp_copy));
  bwrite(bp);
  brelse(bp);
}

// Sync: flush dirty inodes, write imap, write checkpoint
// Called when segment is full, buffer is full, or periodically
void
lfs_sync(void)
{
  // Check if there's anything to sync
  acquire(&dirty_inodes.lock);
  int has_dirty = (dirty_inodes.count > 0);
  release(&dirty_inodes.lock);

  if(!has_dirty)
    return;  // Nothing to sync, avoid unnecessary imap/checkpoint writes

  // 1. Flush dirty inodes to disk (updates in-memory imap)
  lfs_flush_inodes();

  // 2. Write imap to log
  lfs_write_imap();

  // 3. Write checkpoint
  lfs_write_checkpoint();

  cprintf("LFS sync: log_tail now %d\n", lfs.log_tail);
}

// Write imap to log
static void
lfs_write_imap(void)
{
  struct buf *bp;
  uint i, j;
  uint *p;
  uint nblocks = (LFS_NINODES + IMAP_ENTRIES_PER_BLOCK - 1) / IMAP_ENTRIES_PER_BLOCK;
  uint block;
  uint imap_copy[LFS_NINODES];

  // Copy imap under lock
  acquire(&lfs.lock);
  memmove(imap_copy, lfs.imap, sizeof(imap_copy));
  lfs.cp.imap_nblocks = nblocks;
  release(&lfs.lock);

  for(i = 0; i < nblocks && i < NIMAP_BLOCKS; i++){
    // Allocate block for imap
    acquire(&lfs.lock);
    block = lfs.log_tail++;
    if(lfs.log_tail >= sb.size){
      release(&lfs.lock);
      panic("lfs_write_imap: out of disk space");
    }
    lfs.cp.imap_addrs[i] = block;
    release(&lfs.lock);

    // Write imap block (outside lock)
    bp = bread(lfs.dev, block);
    memset(bp->data, 0, BSIZE);
    p = (uint*)bp->data;
    for(j = 0; j < IMAP_ENTRIES_PER_BLOCK && (i * IMAP_ENTRIES_PER_BLOCK + j) < LFS_NINODES; j++){
      p[j] = imap_copy[i * IMAP_ENTRIES_PER_BLOCK + j];
    }
    bwrite(bp);
    brelse(bp);
  }
}

// Allocate a block from the log tail
static uint
lfs_alloc(void)
{
  uint block;

  acquire(&lfs.lock);
  block = lfs.log_tail++;
  if(lfs.log_tail >= sb.size){
    release(&lfs.lock);
    panic("lfs_alloc: out of disk space");
  }
  release(&lfs.lock);

  cprintf("LFS alloc: block %d\n", block);
  return block;
}

// Flush dirty inodes to a single block
// Must be called before checkpoint or when buffer is full
static void
lfs_flush_inodes(void)
{
  struct buf *bp;
  struct dinode *dip;
  uint block;
  int i, count;
  struct dinode inodes_copy[IPB];
  uint inums_copy[IPB];

  // Copy data under lock
  acquire(&dirty_inodes.lock);
  count = dirty_inodes.count;
  if(count == 0){
    release(&dirty_inodes.lock);
    return;
  }
  for(i = 0; i < count; i++){
    memmove(&inodes_copy[i], &dirty_inodes.inodes[i], sizeof(struct dinode));
    inums_copy[i] = dirty_inodes.inums[i];
  }
  dirty_inodes.count = 0;
  release(&dirty_inodes.lock);

  // Allocate a block for the inodes
  cprintf("LFS flush %d inodes:", count);
  for(i = 0; i < count; i++) cprintf(" %d", inums_copy[i]);
  cprintf("\n");
  block = lfs_alloc();

  // Write all inodes to the block
  bp = bread(lfs.dev, block);
  memset(bp->data, 0, BSIZE);
  dip = (struct dinode*)bp->data;
  for(i = 0; i < count; i++){
    memmove(&dip[i], &inodes_copy[i], sizeof(struct dinode));
  }
  bwrite(bp);
  brelse(bp);

  // Update imap with new locations (version starts at 0)
  acquire(&lfs.lock);
  for(i = 0; i < count; i++){
    uint old_entry = lfs.imap[inums_copy[i]];
    uint new_version = (old_entry == 0 || old_entry == 0xFFFFFFFF) ? 0 : (IMAP_VERSION(old_entry) + 1) & IMAP_VERSION_MASK;
    lfs.imap[inums_copy[i]] = IMAP_ENCODE(block, new_version, i);
  }
  release(&lfs.lock);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// In LFS, inodes are NOT at fixed locations. The imap tracks
// where each inode is currently stored in the log.

struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} icache;

void
iinit(int dev)
{
  int i = 0;

  initlock(&icache.lock, "icache");
  for(i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }

  initlock(&lfs.lock, "lfs");
  initlock(&dirty_inodes.lock, "dirty_inodes");
  dirty_inodes.count = 0;
  lfs.dev = dev;

  readsb(dev, &sb);

  // Verify LFS magic
  if(sb.magic != LFS_MAGIC){
    panic("iinit: not an LFS filesystem");
  }

  // Read checkpoint and imap
  lfs_read_checkpoint(dev);
  lfs_read_imap(dev);

  cprintf("LFS: size %d nsegs %d segsize %d segstart %d ninodes %d log_tail %d\n",
          sb.size, sb.nsegs, sb.segsize, sb.segstart, sb.ninodes, lfs.log_tail);
}

static struct inode* iget(uint dev, uint inum);

//PAGEBREAK!
// Allocate an inode on device dev.
// Mark it as allocated by giving it type type.
// Returns an unlocked but allocated and referenced inode.
// Sprite LFS: inode is added to dirty buffer, NOT persisted immediately.
struct inode*
ialloc(uint dev, short type)
{
  int inum;
  struct dinode di;
  int need_sync = 0;

  // Find a free inode slot
  acquire(&lfs.lock);
  for(inum = 1; inum < LFS_NINODES; inum++){
    if(lfs.imap[inum] == 0){
      // Mark slot as used with a placeholder (0xFFFFFFFF means "in dirty buffer")
      lfs.imap[inum] = 0xFFFFFFFF;
      release(&lfs.lock);
      cprintf("ialloc: inum %d type %d\n", inum, type);

      // Initialize inode
      memset(&di, 0, sizeof(di));
      di.type = type;
      di.nlink = 0;
      di.size = 0;

      // Add inode to dirty buffer
      acquire(&dirty_inodes.lock);
      if(dirty_inodes.count >= IPB){
        // Buffer is full - sync before adding
        release(&dirty_inodes.lock);
        lfs_sync();
        acquire(&dirty_inodes.lock);
      }
      memmove(&dirty_inodes.inodes[dirty_inodes.count], &di, sizeof(di));
      dirty_inodes.inums[dirty_inodes.count] = inum;
      dirty_inodes.count++;
      if(dirty_inodes.count >= IPB){
        need_sync = 1;
      }
      release(&dirty_inodes.lock);

      if(need_sync){
        lfs_sync();
      }
      // NO checkpoint here - Sprite LFS approach

      return iget(dev, inum);
    }
  }

  release(&lfs.lock);
  panic("ialloc: no inodes");
}

// Copy a modified in-memory inode to dirty buffer.
// In LFS, inodes are buffered and flushed together when buffer is full.
// This is the Sprite LFS approach: data first, inodes batched later.
// Caller must hold ip->lock.
void
iupdate(struct inode *ip)
{
  struct dinode di;
  int i, found;
  int need_sync = 0;

  // Prepare dinode
  di.type = ip->type;
  di.major = ip->major;
  di.minor = ip->minor;
  di.nlink = ip->nlink;
  di.size = ip->size;
  memmove(di.addrs, ip->addrs, sizeof(ip->addrs));

  acquire(&dirty_inodes.lock);

  // Check if this inode is already in the buffer (update in place)
  found = 0;
  for(i = 0; i < dirty_inodes.count; i++){
    if(dirty_inodes.inums[i] == ip->inum){
      memmove(&dirty_inodes.inodes[i], &di, sizeof(di));
      found = 1;
      break;
    }
  }

  if(!found){
    // Add new inode to buffer
    if(dirty_inodes.count >= IPB){
      // Buffer is full - sync before adding
      release(&dirty_inodes.lock);
      lfs_sync();
      acquire(&dirty_inodes.lock);
    }
    memmove(&dirty_inodes.inodes[dirty_inodes.count], &di, sizeof(di));
    dirty_inodes.inums[dirty_inodes.count] = ip->inum;
    dirty_inodes.count++;
  }

  // Check if we need to sync after adding
  if(dirty_inodes.count >= IPB){
    need_sync = 1;
  }
  release(&dirty_inodes.lock);

  if(need_sync){
    lfs_sync();
  }
  // NO checkpoint here - Sprite LFS approach: sync only when buffer is full
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
static struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for(ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if(empty == 0 && ip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if(empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  release(&icache.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
idup(struct inode *ip)
{
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
// In LFS, we first check the dirty buffer, then look up in imap.
void
ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;
  uint imap_entry;
  uint block;
  uint slot;
  int i, found;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);

  if(ip->valid == 0){
    // First check if inode is in dirty buffer
    found = 0;
    acquire(&dirty_inodes.lock);
    for(i = 0; i < dirty_inodes.count; i++){
      if(dirty_inodes.inums[i] == ip->inum){
        // Found in dirty buffer - copy from there
        dip = &dirty_inodes.inodes[i];
        ip->type = dip->type;
        ip->major = dip->major;
        ip->minor = dip->minor;
        ip->nlink = dip->nlink;
        ip->size = dip->size;
        memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
        found = 1;
        break;
      }
    }
    release(&dirty_inodes.lock);

    if(!found){
      // Look up inode location in imap
      acquire(&lfs.lock);
      imap_entry = lfs.imap[ip->inum];
      release(&lfs.lock);

      if(imap_entry == 0){
        cprintf("ilock: inum %d not in imap\n", ip->inum);
        panic("ilock: inode not in imap");
      }

      // Check for placeholder (inode should be in dirty buffer but wasn't found)
      if(imap_entry == 0xFFFFFFFF)
        panic("ilock: inode marked in-flight but not in dirty buffer");

      // Decode block address and slot from imap entry
      block = IMAP_BLOCK(imap_entry);
      slot = IMAP_SLOT(imap_entry);

      bp = bread(ip->dev, block);
      dip = (struct dinode*)bp->data + slot;
      ip->type = dip->type;
      ip->major = dip->major;
      ip->minor = dip->minor;
      ip->nlink = dip->nlink;
      ip->size = dip->size;
      memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
      brelse(bp);
    }

    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
void
iput(struct inode *ip)
{
  int i;

  acquiresleep(&ip->lock);
  if(ip->valid && ip->nlink == 0){
    acquire(&icache.lock);
    int r = ip->ref;
    release(&icache.lock);
    if(r == 1){
      // inode has no links and no other references: truncate and free.
      itrunc(ip);
      ip->type = 0;

      // Remove inode from dirty buffer if present (don't need to persist type=0)
      acquire(&dirty_inodes.lock);
      for(i = 0; i < dirty_inodes.count; i++){
        if(dirty_inodes.inums[i] == ip->inum){
          // Remove by shifting remaining entries
          for(; i < dirty_inodes.count - 1; i++){
            memmove(&dirty_inodes.inodes[i], &dirty_inodes.inodes[i+1], sizeof(struct dinode));
            dirty_inodes.inums[i] = dirty_inodes.inums[i+1];
          }
          dirty_inodes.count--;
          break;
        }
      }
      release(&dirty_inodes.lock);

      // Mark inode as free in imap
      acquire(&lfs.lock);
      lfs.imap[ip->inum] = 0;
      release(&lfs.lock);

      // Sync to persist the freed inode slot
      lfs_sync();

      ip->valid = 0;
    }
  }
  releasesleep(&ip->lock);

  acquire(&icache.lock);
  ip->ref--;
  release(&icache.lock);
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

//PAGEBREAK!
// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
// In LFS, new blocks are allocated from the log tail.
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0){
      ip->addrs[bn] = addr = lfs_alloc();
      cprintf("  bmap: inum %d bn %d -> block %d\n", ip->inum, bn, addr);
    }
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    // Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0)
      ip->addrs[NDIRECT] = addr = lfs_alloc();
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      a[bn] = addr = lfs_alloc();
      bwrite(bp);
    }
    brelse(bp);
    return addr;
  }

  panic("bmap: out of range");
}

// Truncate inode (discard contents).
// In LFS without GC, we don't actually free blocks - just clear the references.
static void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp;
  uint *a;

  // Clear direct blocks (don't free - no GC)
  for(i = 0; i < NDIRECT; i++){
    ip->addrs[i] = 0;
  }

  // Clear indirect block
  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      a[j] = 0;
    }
    brelse(bp);
    ip->addrs[NDIRECT] = 0;
  }

  ip->size = 0;
  iupdate(ip);
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

//PAGEBREAK!
// Read data from inode.
// Caller must hold ip->lock.
int
readi(struct inode *ip, char *dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].read)
      return -1;
    return devsw[ip->major].read(ip, dst, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > ip->size)
    n = ip->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE));
    m = min(n - tot, BSIZE - off%BSIZE);
    memmove(dst, bp->data + off%BSIZE, m);
    brelse(bp);
  }
  return n;
}

// PAGEBREAK!
// Write data to inode.
// Caller must hold ip->lock.
// In LFS, data blocks are written to the log.
int
writei(struct inode *ip, char *src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].write)
      return -1;
    return devsw[ip->major].write(ip, src, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > MAXFILE*BSIZE)
    return -1;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE));
    m = min(n - tot, BSIZE - off%BSIZE);
    memmove(bp->data + off%BSIZE, src, m);
    bwrite(bp);
    brelse(bp);
  }

  if(n > 0 && off > ip->size){
    ip->size = off;
  }

  // Update inode in log (writes new inode, imap, checkpoint)
  iupdate(ip);

  return n;
}

//PAGEBREAK!
// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      // entry matches path element
      if(poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("dirlink");

  return 0;
}

//PAGEBREAK!
// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(myproc()->cwd);

  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}
