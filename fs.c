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

// Special value to mark segment as free (prevents re-selection by GC)
#define SUT_FREE_MARKER 0xFFFFFFFF

static void itrunc(struct inode*);
static void lfs_flush_inodes(void);
static void lfs_write_imap(void);
static void lfs_write_sut(void);
void lfs_flush_ssb_inline(void);  // Legacy compatibility
static uint lfs_write_ssb_now(void);  // Write SSB to log immediately
static uint gc_compute_checksum(struct ssb_entry *entries, int count);
static void lfs_gc(void);
static uint lfs_alloc(void);  // Allocate block from log (no SSB entry)
static uint lfs_alloc_with_ssb(uchar ssb_type, uint ssb_inum, uint ssb_offset, uint ssb_version);  // Allocate with atomic SSB entry
static void lfs_write_pending_ssb(void);  // Write pending SSB

// There should be one superblock per disk device, but we run with only one device
struct superblock sb;

// LFS state
struct {
  struct spinlock lock;
  uint imap[LFS_NINODES];      // inode number -> IMAP_ENCODE(block, version, slot)
  struct checkpoint cp;        // current checkpoint
  uint log_tail;               // next block to write
  uint cur_seg_end;            // end of current valid allocation region
  int dev;                     // device number
  int syncing;                 // recursion guard
  // GC / SUT state
  struct sut_entry sut[LFS_NSEGS_MAX]; // Segment Usage Table
  struct ssb_entry ssb_buf[SSB_ENTRIES_PER_BLOCK]; // Buffer for current segment's SSBs
  uint ssb_count;              // Number of entries in ssb_buf
  uint ssb_seg_start;          // Start block of segment that SSB entries belong to
  int ssb_flushing;            // SSB flush in progress flag (prevent recursion)
  struct ssb_entry ssb_flush_buf[SSB_ENTRIES_PER_BLOCK]; // Separate buffer for flushing (to avoid races)
  uint ssb_pending_block;      // Block allocated for pending SSB (to write after alloc returns)
  uint reserved_ssb_block;     // Explicitly reserved block for SSB (at end of segment)
  int ssb_pending_count;       // Number of entries in pending SSB
  // GC free segment list (circular buffer)
  uint free_segs[LFS_NSEGS_MAX];  // Free segment indices
  int free_head;                   // Free list head index
  int free_tail;                   // Free list tail index
  int free_count;                  // Number of free segments
  int gc_running;                  // GC recursion guard
  int gc_failed;                   // GC found no segments last run
  
  // Pending free segments (waiting for checkpoint sync)
  uint pending_free_segs[GC_TARGET_SEGS];
  int pending_free_count;
} lfs;

// Dirty inode buffer - holds up to IPB (16) inodes before writing
struct {
  struct spinlock lock;
  // Active buffer
  struct dinode inodes[IPB];
  uint inums[IPB];
  uint versions[IPB];
  int count;
  // Flushing buffer
  struct dinode flushing_inodes[IPB];
  uint flushing_inums[IPB];
  uint flushing_versions[IPB];
  int flushing_count;
} dirty_inodes;

// In-memory inode cache
struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} icache;

// Read the super block.
void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Read SUT from disk (locations stored in checkpoint)
static void
lfs_read_sut(int dev)
{
  struct buf *bp;
  uint i, j;
  struct sut_entry *p;

  // Initialize SUT
  memset(lfs.sut, 0, sizeof(lfs.sut));

  for(i = 0; i < lfs.cp.sut_nblocks && i < NSUT_BLOCKS; i++){
    if(lfs.cp.sut_addrs[i] == 0) continue; // Skip invalid address
    bp = bread(dev, lfs.cp.sut_addrs[i]);
    p = (struct sut_entry*)bp->data;
    // Calculate how many entries fit in one block
    int entries_per_block = BSIZE / sizeof(struct sut_entry);
    for(j = 0; j < entries_per_block && (i * entries_per_block + j) < LFS_NSEGS_MAX; j++){
      lfs.sut[i * entries_per_block + j] = p[j];
    }
    brelse(bp);
  }
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
    // First boot? Initialize log_tail
    lfs.log_tail = sb.segstart;
    cprintf("lfs_read_checkpoint: invalid checkpoint (first boot?)\n");
    return;
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

// Write SUT to log (Dynamic size & Partial update)
static void
lfs_write_sut(void)
{
  struct buf *bp;
  uint i, j;
  struct sut_entry *p;
  int entries_per_block = BSIZE / sizeof(struct sut_entry);
  
  // 1. Dynamic Size: Use sb.nsegs instead of LFS_NSEGS_MAX
  uint nsegs_to_write = sb.nsegs;
  if(nsegs_to_write > LFS_NSEGS_MAX) nsegs_to_write = LFS_NSEGS_MAX;
  uint nblocks = (nsegs_to_write + entries_per_block - 1) / entries_per_block;
  
  uint block;
  // Use static to avoid stack overflow
  static struct sut_entry sut_copy[LFS_NSEGS_MAX];
  char block_data[BSIZE]; // Temp buffer for comparison/writing

  // Copy SUT under lock
  acquire(&lfs.lock);
  memmove(sut_copy, lfs.sut, sizeof(sut_copy));
  lfs.cp.sut_nblocks = nblocks;
  release(&lfs.lock);

  for(i = 0; i < nblocks && i < NSUT_BLOCKS; i++){
    // Prepare new block data
    memset(block_data, 0, BSIZE);
    p = (struct sut_entry*)block_data;
    for(j = 0; j < entries_per_block && (i * entries_per_block + j) < nsegs_to_write; j++){
      p[j] = sut_copy[i * entries_per_block + j];
    }

    // 2. Partial Update: Check against old block
    uint old_addr;
    acquire(&lfs.lock);
    old_addr = lfs.cp.sut_addrs[i];
    release(&lfs.lock);

    int need_write = 1;
    if(old_addr != 0){
      bp = bread(lfs.dev, old_addr);
      if(memcmp(bp->data, block_data, BSIZE) == 0){
        need_write = 0;
      }
      brelse(bp);
    }

    if(need_write){
        // Allocate block for SUT
        acquire(&lfs.lock);
        // Check if we need to switch to a free segment
        if(lfs.log_tail >= lfs.cur_seg_end){
          if(lfs.free_count > 0){
            uint free_seg = lfs.free_segs[lfs.free_head];
            lfs.free_head = (lfs.free_head + 1) % LFS_NSEGS_MAX;
            lfs.free_count--;
            lfs.log_tail = sb.segstart + free_seg * sb.segsize;
            lfs.cur_seg_end = lfs.log_tail + sb.segsize;
            lfs.sut[free_seg].live_bytes = 0;

          } else {
            release(&lfs.lock);
            panic("lfs_write_sut: out of disk space (no free segments)");
          }
        }
        block = lfs.log_tail++;
        lfs.cp.sut_addrs[i] = block;
        release(&lfs.lock);

        // Write SUT block (outside lock)
        bp = bread(lfs.dev, block);
        memmove(bp->data, block_data, BSIZE);
        bwrite(bp);
        brelse(bp);
    }
  }
}

// Add entry to SSB buffer
// Called when a data block is written
void
lfs_add_ssb_entry(uchar type, uint inum, uint offset, uint version)
{
  acquire(&lfs.lock);
  
  // With LFS_SEGSIZE=32 and SSB capacity ~63, overflow is structurally impossible
  // if we write 1 entry per block. We keep a basic check just in case.
  if(lfs.ssb_count < SSB_ENTRIES_PER_BLOCK){
    lfs.ssb_buf[lfs.ssb_count].type = type;
    lfs.ssb_buf[lfs.ssb_count].inum = inum;
    lfs.ssb_buf[lfs.ssb_count].offset = offset;
    lfs.ssb_buf[lfs.ssb_count].version = version;
    lfs.ssb_count++;
  }
  
  release(&lfs.lock);
}

// Write current SSB entries to log NOW (unconditionally)
// Returns the block number where SSB was written, or 0 if nothing to write or out of space
// MUST be called WITHOUT holding lfs.lock
static uint
lfs_write_ssb_now(void)
{
  struct buf *bp;
  struct ssb *ssb_ptr;
  uint block = 0;
  int count;

  acquire(&lfs.lock);

  // Check if another flush is in progress
  if(lfs.ssb_flushing){
    release(&lfs.lock);
    return 0;
  }

  count = lfs.ssb_count;
  if(count == 0){
    release(&lfs.lock);
    return 0;
  }

  // Mark as flushing and copy to flush buffer
  lfs.ssb_flushing = 1;
  memmove(lfs.ssb_flush_buf, lfs.ssb_buf, count * sizeof(struct ssb_entry));
  lfs.ssb_count = 0;

  // Use explicitly reserved block if available (from lfs_alloc segment switch)
  if(lfs.reserved_ssb_block != 0){
    block = lfs.reserved_ssb_block;
    lfs.reserved_ssb_block = 0;
  } else {
    // Check if we need to switch to a free segment
    if(lfs.log_tail >= lfs.cur_seg_end){
      if(lfs.free_count > 0){
        uint free_seg = lfs.free_segs[lfs.free_head];
        lfs.free_head = (lfs.free_head + 1) % LFS_NSEGS_MAX;
        lfs.free_count--;
        lfs.log_tail = sb.segstart + free_seg * sb.segsize;
        lfs.cur_seg_end = lfs.log_tail + sb.segsize;
        lfs.sut[free_seg].live_bytes = 0;
      } else {
        // Out of space - restore SSB entries and return 0
        // This can happen during GC when disk is critically full
        memmove(lfs.ssb_buf, lfs.ssb_flush_buf, count * sizeof(struct ssb_entry));
        lfs.ssb_count = count;
        lfs.ssb_flushing = 0;
        release(&lfs.lock);
        cprintf("lfs_write_ssb_now: out of space, deferring SSB write\n");
        return 0;
      }
    }
    block = lfs.log_tail++;
  }
  release(&lfs.lock);

  // Write SSB block (outside lock)
  bp = bread(lfs.dev, block);
  memset(bp->data, 0, BSIZE);
  ssb_ptr = (struct ssb *)bp->data;
  ssb_ptr->magic = SSB_MAGIC;
  ssb_ptr->nblocks = count;
  ssb_ptr->checksum = gc_compute_checksum(lfs.ssb_flush_buf, count);
  memmove(ssb_ptr->entries, lfs.ssb_flush_buf, count * sizeof(struct ssb_entry));
  bwrite(bp);
  brelse(bp);

  // Clear flushing flag
  acquire(&lfs.lock);
  lfs.ssb_flushing = 0;
  release(&lfs.lock);

  return block;
}

// Legacy function for compatibility
void
lfs_flush_ssb_inline(void)
{
  lfs_write_ssb_now();
}

// Write pending SSB that was prepared during segment switch
// MUST be called WITHOUT holding lfs.lock
// Called from writei() after lfs_alloc() returns
static void
lfs_write_pending_ssb(void)
{
  uint block;
  int count;

  acquire(&lfs.lock);
  if(!lfs.ssb_flushing || lfs.ssb_pending_count == 0){
    release(&lfs.lock);
    return;
  }
  block = lfs.ssb_pending_block;
  count = lfs.ssb_pending_count;
  release(&lfs.lock);

  // Write SSB block (outside lock)
  struct buf *bp = bread(lfs.dev, block);
  memset(bp->data, 0, BSIZE);
  struct ssb *ssb_ptr = (struct ssb *)bp->data;
  ssb_ptr->magic = SSB_MAGIC;
  ssb_ptr->nblocks = count;
  ssb_ptr->checksum = gc_compute_checksum(lfs.ssb_flush_buf, count);
  memmove(ssb_ptr->entries, lfs.ssb_flush_buf, count * sizeof(struct ssb_entry));
  bwrite(bp);
  brelse(bp);

  // Clear pending state
  acquire(&lfs.lock);
  lfs.ssb_flushing = 0;
  lfs.ssb_pending_count = 0;
  // If we just wrote to the reserved block, clear the reservation
  // so subsequent writes don't overwrite it with new segment data.
  if(lfs.reserved_ssb_block == block){
    lfs.reserved_ssb_block = 0;
  }
  release(&lfs.lock);
}

// Check if we need to flush SSB before allocation and prepare if so
// MUST be called WITHOUT holding lfs.lock
// Returns 1 if SSB needs to be written (call lfs_write_pending_ssb after alloc)
static int
lfs_prepare_alloc(void)
{
  acquire(&lfs.lock);

  // Already flushing?
  if(lfs.ssb_flushing){
    release(&lfs.lock);
    return 1;  // Caller should write pending SSB
  }

  uint seg_offset = (lfs.log_tail - sb.segstart) % sb.segsize;
  uint seg_remaining = sb.segsize - seg_offset;

  // CRITICAL: If only 1 block remains in segment and we have SSB entries,
  // write SSB to this LAST block. This ensures SSB is always the final
  // block in a segment, guaranteeing complete coverage.
  if(seg_remaining == 1 && lfs.ssb_count > 0){
    lfs.ssb_flushing = 1;
    lfs.ssb_pending_count = lfs.ssb_count;
    memmove(lfs.ssb_flush_buf, lfs.ssb_buf, lfs.ssb_count * sizeof(struct ssb_entry));
    lfs.ssb_count = 0;
    // Allocate the LAST block for SSB
    lfs.ssb_pending_block = lfs.log_tail++;
    release(&lfs.lock);
    return 1;  // Caller should write pending SSB
  }

  release(&lfs.lock);
  return 0;
}

// Update segment usage
// Called when block is allocated (+BSIZE) or freed/overwritten (-BSIZE)
void
lfs_update_usage(uint block_addr, int delta)
{
  uint seg_idx;

  if(block_addr < sb.segstart) return; // Not in log area

  seg_idx = (block_addr - sb.segstart) / sb.segsize;
  
  acquire(&lfs.lock);
  if(seg_idx < LFS_NSEGS_MAX){
    if(delta > 0){
      lfs.sut[seg_idx].live_bytes += delta;
    } else {
      if(lfs.sut[seg_idx].live_bytes >= -delta)
        lfs.sut[seg_idx].live_bytes += delta;
      else
        lfs.sut[seg_idx].live_bytes = 0;
    }
    // Update age (simple ticks)
    lfs.sut[seg_idx].age = ticks;
  }
  release(&lfs.lock);
}

// ============================================================================
// Garbage Collection (Segment Cleaning) Implementation
// Based on Sprite LFS paper: cost-benefit policy + UID-based live detection
// ============================================================================

// Compute checksum for SSB entries (simple XOR-based)
static uint
gc_compute_checksum(struct ssb_entry *entries, int count)
{
  uint checksum = 0;
  int i;
  uint *p;

  for(i = 0; i < count; i++){
    p = (uint*)&entries[i];
    checksum ^= p[0] ^ p[1] ^ p[2];  // XOR inum, offset, version
  }
  return checksum;
}

// Verify SSB checksum
static int
gc_verify_checksum(struct ssb *ssb_ptr)
{
  uint computed = gc_compute_checksum(ssb_ptr->entries, ssb_ptr->nblocks);
  return (computed == ssb_ptr->checksum);
}

// Calculate cost-benefit score for segment cleaning
// Score = (1 - u) * age / (1 + u)
// Higher score = better candidate for cleaning
// u = utilization (live_bytes / segment_size_bytes)
static uint
gc_cost_benefit(uint seg_idx)
{
  uint live_bytes;
  uint age;
  uint seg_size_bytes = sb.segsize * BSIZE;
  uint u_percent;  // utilization as percentage (0-100)
  uint score;

  acquire(&lfs.lock);
  live_bytes = lfs.sut[seg_idx].live_bytes;
  age = lfs.sut[seg_idx].age;
  release(&lfs.lock);

  // Calculate utilization percentage
  if(live_bytes >= seg_size_bytes){
    u_percent = 100;
  } else {
    u_percent = (live_bytes * 100) / seg_size_bytes;
  }

  // Age factor: use ticks difference (older = higher age value)
  uint age_factor = (ticks > age) ? (ticks - age) : 1;
  if(age_factor == 0) age_factor = 1;

  // Score = (100 - u) * age / (100 + u)
  // Multiply by 1000 for better precision
  if(u_percent >= 100){
    score = 0;  // Fully utilized segment - don't clean
  } else {
    score = ((100 - u_percent) * age_factor * 1000) / (100 + u_percent);
  }

  return score;
}

// Structure for victim selection
struct gc_victim {
  uint seg_idx;
  uint score;
  uint util_percent;
};

// Select victim segments for cleaning
// Returns number of victims selected (up to max_victims)
static int
gc_select_victims(struct gc_victim *victims, int max_victims)
{
  int i, j, k;
  uint cur_seg;
  uint seg_size_bytes = sb.segsize * BSIZE;
  int victim_count = 0;

  // Get current segment (exclude from cleaning)
  acquire(&lfs.lock);
  cur_seg = (lfs.log_tail - sb.segstart) / sb.segsize;
  release(&lfs.lock);

  // Scan all segments, skipping the current one being written to
  for(i = 0; i < sb.nsegs && i < LFS_NSEGS_MAX; i++){
    if(i == cur_seg) continue;  // Skip segment we're currently writing to
    // Check utilization threshold
    uint live_bytes;
    acquire(&lfs.lock);
    live_bytes = lfs.sut[i].live_bytes;
    release(&lfs.lock);

    // Skip free segments (marked with special value)
    if(live_bytes == SUT_FREE_MARKER) continue;

    uint util_percent = (live_bytes * 100) / seg_size_bytes;
    
    // Calculate score
    uint score = gc_cost_benefit(i);
    
    // If we are desperate (no victims yet), take anything that isn't full/free
    if(victim_count == 0 && util_percent < 100 && score == 0) score = 1;

    if(score == 0) continue;

    // Insert into victims array (sorted by score, descending)
    struct gc_victim new_victim = {i, score, util_percent};

    if(victim_count < max_victims){
      // Find insertion position
      for(j = 0; j < victim_count; j++){
        if(score > victims[j].score) break;
      }
      // Shift elements
      for(k = victim_count; k > j; k--){
        victims[k] = victims[k-1];
      }
      victims[j] = new_victim;
      victim_count++;
    } else if(score > victims[max_victims-1].score){
      // Replace lowest scoring victim
      for(j = 0; j < max_victims; j++){
        if(score > victims[j].score) break;
      }
      // Shift elements
      for(k = max_victims - 1; k > j; k--){
        victims[k] = victims[k-1];
      }
      victims[j] = new_victim;
    }
  }

  return victim_count;
}

// Find SSB blocks within a segment by scanning for magic number
// Returns number of SSBs found, fills ssb_addrs array
static int
gc_find_ssbs(uint seg_idx, uint *ssb_addrs, int max_ssbs)
{
  uint seg_start = sb.segstart + seg_idx * sb.segsize;
  uint seg_end = seg_start + sb.segsize;
  struct buf *bp;
  struct ssb *ssb_ptr;
  int count = 0;
  uint blk;

  for(blk = seg_start; blk < seg_end && count < max_ssbs; blk++){
    bp = bread(lfs.dev, blk);
    ssb_ptr = (struct ssb *)bp->data;

    if(ssb_ptr->magic == SSB_MAGIC){
      // Verify checksum
      if(gc_verify_checksum(ssb_ptr)){
        ssb_addrs[count++] = blk;
      }
    }
    brelse(bp);
  }

  return count;
}

// Add segment to pending free list
// Will be moved to real free list after checkpoint sync
static void
gc_free_segment(uint seg_idx)
{
  // Validate segment index
  if(seg_idx >= sb.nsegs){
    cprintf("gc_free_segment: INVALID seg_idx %d >= nsegs %d\n", seg_idx, sb.nsegs);
    panic("gc_free_segment: invalid segment index");
  }

  acquire(&lfs.lock);
  // Add directly to free list (needed for sync operations during GC)
  if(lfs.free_count < LFS_NSEGS_MAX){
    lfs.free_segs[lfs.free_tail] = seg_idx;
    lfs.free_tail = (lfs.free_tail + 1) % LFS_NSEGS_MAX;
    lfs.free_count++;
  }
  // Mark as free with special value (so GC won't re-select it)
  lfs.sut[seg_idx].live_bytes = SUT_FREE_MARKER;
  lfs.sut[seg_idx].age = ticks;
  release(&lfs.lock);
}

// Relocate an inode block to the current log tail
// Updates imap entries for all inodes in the block
// Returns 0 on success, -1 on failure (out of space)
static int
gc_relocate_inode_block(uint inum, uint old_block)
{
  struct buf *bp_old, *bp_new;
  uint new_block;
  int i;

  // 0. Validate old_block
  if(old_block >= sb.size){
    cprintf("gc_relocate_inode_block: INVALID old_block=%d >= size=%d (inum=%d)\n",
            old_block, sb.size, inum);
    return -1;
  }

  // 1. Read old inode block
  bp_old = bread(lfs.dev, old_block);

  // 2. Allocate new block at log tail
  acquire(&lfs.lock);
  if(lfs.log_tail >= lfs.cur_seg_end){
    if(lfs.free_count > 0){
      uint free_seg = lfs.free_segs[lfs.free_head];
      lfs.free_head = (lfs.free_head + 1) % LFS_NSEGS_MAX;
      lfs.free_count--;
      lfs.log_tail = sb.segstart + free_seg * sb.segsize;
      lfs.cur_seg_end = lfs.log_tail + sb.segsize;
      lfs.sut[free_seg].live_bytes = 0;
    } else {
      release(&lfs.lock);
      brelse(bp_old);
      return -1;  // Out of space
    }
  }
  new_block = lfs.log_tail++;

  // Find first inum in this block for SSB entry
  uint first_inum = 0;
  for(i = 0; i < LFS_NINODES; i++){
    uint entry = lfs.imap[i];
    if(entry != 0 && entry != 0xFFFFFFFF){
      if(IMAP_BLOCK(entry) == old_block){
        first_inum = i;
        break;
      }
    }
  }

  // Add single SSB entry for the inode block (GC will check all imaps)
  if(lfs.ssb_count < SSB_ENTRIES_PER_BLOCK){
    lfs.ssb_buf[lfs.ssb_count].type = SSB_TYPE_INODE;
    lfs.ssb_buf[lfs.ssb_count].inum = first_inum;
    lfs.ssb_buf[lfs.ssb_count].offset = 0;
    lfs.ssb_buf[lfs.ssb_count].version = 0;  // Not used for block-level check
    lfs.ssb_count++;
  }
  release(&lfs.lock);

  // 3. Write inode block to new location
  // IMPORTANT: Apply any dirty buffer updates to the copied data
  bp_new = bread(lfs.dev, new_block);
  memmove(bp_new->data, bp_old->data, BSIZE);

  // Merge dirty buffer inodes that belong to this block
  acquire(&dirty_inodes.lock);
  struct dinode *new_dips = (struct dinode*)bp_new->data;
  for(int di = 0; di < dirty_inodes.count; di++){
    uint inum = dirty_inodes.inums[di];
    uint imap_entry = lfs.imap[inum];  // Note: lfs.lock already released, but reading is OK
    if(imap_entry != 0 && imap_entry != 0xFFFFFFFF){
      if(IMAP_BLOCK(imap_entry) == old_block){
        // This dirty inode belongs to the block being relocated
        uint slot = IMAP_SLOT(imap_entry);
        if(slot < IPB){
          memmove(&new_dips[slot], &dirty_inodes.inodes[di], sizeof(struct dinode));
        }
      }
    }
  }
  // Also check flushing buffer
  for(int di = 0; di < dirty_inodes.flushing_count; di++){
    uint inum = dirty_inodes.flushing_inums[di];
    uint imap_entry = lfs.imap[inum];
    if(imap_entry != 0 && imap_entry != 0xFFFFFFFF){
      if(IMAP_BLOCK(imap_entry) == old_block){
        uint slot = IMAP_SLOT(imap_entry);
        if(slot < IPB){
          memmove(&new_dips[slot], &dirty_inodes.flushing_inodes[di], sizeof(struct dinode));
        }
      }
    }
  }
  release(&dirty_inodes.lock);

  bwrite(bp_new);
  brelse(bp_new);
  brelse(bp_old);

  // 4. Update SUT
  lfs_update_usage(new_block, BSIZE);
  lfs_update_usage(old_block, -BSIZE);

  // 5. Update imap for all inodes that point to old_block
  acquire(&lfs.lock);
  for(i = 0; i < LFS_NINODES; i++){
    uint entry = lfs.imap[i];
    if(entry != 0 && entry != 0xFFFFFFFF){
      if(IMAP_BLOCK(entry) == old_block){
        uint version = IMAP_VERSION(entry);
        uint slot = IMAP_SLOT(entry);
        lfs.imap[i] = IMAP_ENCODE(new_block, version, slot);
      }
    }
  }
  release(&lfs.lock);

  return 0;  // Success
}

// Relocate a live block to the current log tail
// Updates inode's addrs[] to point to new location
// NOTE: Lock ordering must be lfs.lock -> dirty_inodes.lock to avoid deadlock
// Returns 0 on success, -1 on failure (out of space)
static int
gc_relocate_block(struct ssb_entry *entry, uint old_block)
{
  struct buf *bp_old, *bp_new, *bp_inode;
  struct dinode *dip;
  uint new_block;
  uint imap_entry;
  uint inode_block, inode_slot;
  uint bn;
  uint current_version;
  int found_in_dirty = 0;
  int dirty_idx = -1;

  // 0. Validate old_block before any operation
  if(old_block >= sb.size){
    cprintf("gc_relocate_block: INVALID old_block=%d >= size=%d (inum=%d)\n",
            old_block, sb.size, entry->inum);
    return -1;
  }

  // 1. Get current version from imap (NOT the old SSB entry version!)
  acquire(&lfs.lock);
  imap_entry = lfs.imap[entry->inum];
  release(&lfs.lock);
  current_version = IMAP_VERSION(imap_entry);

  // 2. Read old block data
  bp_old = bread(lfs.dev, old_block);

  // 3. Allocate new block at log tail with atomic SSB entry
  acquire(&lfs.lock);
  // Check if we need to switch to a free segment
  if(lfs.log_tail >= lfs.cur_seg_end){
    if(lfs.free_count > 0){
      uint free_seg = lfs.free_segs[lfs.free_head];
      lfs.free_head = (lfs.free_head + 1) % LFS_NSEGS_MAX;
      lfs.free_count--;
      lfs.log_tail = sb.segstart + free_seg * sb.segsize;
      lfs.cur_seg_end = lfs.log_tail + sb.segsize;
      lfs.sut[free_seg].live_bytes = 0;
    } else {
      release(&lfs.lock);
      brelse(bp_old);
      // Return error instead of panicking - GC will stop early
      return -1;
    }
  }
  new_block = lfs.log_tail++;

  // Atomically add SSB entry for relocated block (while still holding lock)
  if(lfs.ssb_count < SSB_ENTRIES_PER_BLOCK){
    lfs.ssb_buf[lfs.ssb_count].type = entry->type;
    lfs.ssb_buf[lfs.ssb_count].inum = entry->inum;
    lfs.ssb_buf[lfs.ssb_count].offset = entry->offset;
    lfs.ssb_buf[lfs.ssb_count].version = current_version;
    lfs.ssb_count++;
  }
  release(&lfs.lock);

  // 4. Write data to new block
  bp_new = bread(lfs.dev, new_block);
  memmove(bp_new->data, bp_old->data, BSIZE);
  bwrite(bp_new);
  brelse(bp_new);
  brelse(bp_old);

  // 5. Update SUT: new block is live, old block is dead
  lfs_update_usage(new_block, BSIZE);
  lfs_update_usage(old_block, -BSIZE);

  // 6. Update inode's addrs[] to point to new block
  bn = entry->offset;

  // Validate bn
  if(bn >= MAXFILE){
    cprintf("gc_relocate_block: INVALID bn=%d >= MAXFILE=%d (inum=%d, type=%d)\n",
            bn, MAXFILE, entry->inum, entry->type);
    return -1;  // Skip this block instead of corrupting data
  }

  // Check if inode is in dirty buffer (just check, don't modify yet)
  acquire(&dirty_inodes.lock);
  for(int i = 0; i < dirty_inodes.count; i++){
    if(dirty_inodes.inums[i] == entry->inum){
      found_in_dirty = 1;
      dirty_idx = i;
      break;
    }
  }
  release(&dirty_inodes.lock);

  if(found_in_dirty){
    // Inode is in dirty buffer - update it directly
    // NOTE: We must re-find the index each time we acquire lock because
    // dirty buffer might have been flushed between lock releases.

    // First check if inode is freed (type == 0)
    acquire(&dirty_inodes.lock);
    // Re-find dirty_idx (buffer may have changed)
    dirty_idx = -1;
    for(int i = 0; i < dirty_inodes.count; i++){
      if(dirty_inodes.inums[i] == entry->inum){
        dirty_idx = i;
        break;
      }
    }
    if(dirty_idx < 0 || dirty_inodes.inodes[dirty_idx].type == 0){
      release(&dirty_inodes.lock);
      // Inode no longer in dirty buffer or was freed - fall through to disk path
      found_in_dirty = 0;
      goto read_from_disk;
    }
    release(&dirty_inodes.lock);

    // For direct blocks or INDIRECT block type, just update addrs[] directly
    // For DATA blocks accessed through indirect, we need to handle COW
    uint new_ind_for_icache = 0;
    if(bn < NDIRECT || entry->type == SSB_TYPE_INDIRECT){
      acquire(&dirty_inodes.lock);
      // Re-find dirty_idx again
      dirty_idx = -1;
      for(int i = 0; i < dirty_inodes.count; i++){
        if(dirty_inodes.inums[i] == entry->inum){
          dirty_idx = i;
          break;
        }
      }
      if(dirty_idx >= 0){
        if(entry->type == SSB_TYPE_INDIRECT){
          // INDIRECT block: update addrs[NDIRECT]
          dirty_inodes.inodes[dirty_idx].addrs[NDIRECT] = new_block;
        } else {
          // Direct data block: update addrs[bn]
          dirty_inodes.inodes[dirty_idx].addrs[bn] = new_block;
        }
      }
      release(&dirty_inodes.lock);
      if(dirty_idx < 0) goto read_from_disk;
    } else {
      // Indirect block case - read old indirect, create new, update
      uint old_ind;
      acquire(&dirty_inodes.lock);
      // Re-find dirty_idx
      dirty_idx = -1;
      for(int i = 0; i < dirty_inodes.count; i++){
        if(dirty_inodes.inums[i] == entry->inum){
          dirty_idx = i;
          break;
        }
      }
      if(dirty_idx < 0){
        release(&dirty_inodes.lock);
        goto read_from_disk;
      }
      old_ind = dirty_inodes.inodes[dirty_idx].addrs[NDIRECT];
      release(&dirty_inodes.lock);

      if(old_ind != 0){
        struct buf *bp_ind, *bp_new_ind;
        uint new_ind;

        acquire(&lfs.lock);
        if(lfs.log_tail >= lfs.cur_seg_end){
          if(lfs.free_count > 0){
            uint free_seg = lfs.free_segs[lfs.free_head];
            lfs.free_head = (lfs.free_head + 1) % LFS_NSEGS_MAX;
            lfs.free_count--;
            lfs.log_tail = sb.segstart + free_seg * sb.segsize;
            lfs.cur_seg_end = lfs.log_tail + sb.segsize;
            lfs.sut[free_seg].live_bytes = 0;
          } else {
            // Out of space for indirect block - cannot continue
            release(&lfs.lock);
            return -1;
          }
        }
        new_ind = lfs.log_tail++;

        // Atomically add SSB entry for new indirect block
        if(lfs.ssb_count < SSB_ENTRIES_PER_BLOCK){
          lfs.ssb_buf[lfs.ssb_count].type = SSB_TYPE_INDIRECT;
          lfs.ssb_buf[lfs.ssb_count].inum = entry->inum;
          lfs.ssb_buf[lfs.ssb_count].offset = NDIRECT;
          lfs.ssb_buf[lfs.ssb_count].version = current_version;
          lfs.ssb_count++;
        }
        release(&lfs.lock);

        // Validate old_ind before reading
        if(old_ind >= sb.size){
          cprintf("gc_relocate_block: INVALID old_ind=%d >= size=%d (inum=%d)\n",
                  old_ind, sb.size, entry->inum);
          return -1;
        }

        bp_ind = bread(lfs.dev, old_ind);
        bp_new_ind = bread(lfs.dev, new_ind);
        memmove(bp_new_ind->data, bp_ind->data, BSIZE);
        if(entry->type == SSB_TYPE_DATA){
          uint *a = (uint*)bp_new_ind->data;
          // Validate index into indirect block
          if(bn < NDIRECT || bn - NDIRECT >= NINDIRECT){
            cprintf("gc_relocate_block: INVALID indirect index bn=%d (NDIRECT=%d, NINDIRECT=%d)\n",
                    bn, NDIRECT, NINDIRECT);
            brelse(bp_new_ind);
            brelse(bp_ind);
            return -1;
          }
          a[bn - NDIRECT] = new_block;
        }
        bwrite(bp_new_ind);
        brelse(bp_new_ind);
        brelse(bp_ind);

        lfs_update_usage(new_ind, BSIZE);
        lfs_update_usage(old_ind, -BSIZE);

        acquire(&dirty_inodes.lock);
        // Re-find dirty_idx
        dirty_idx = -1;
        for(int i = 0; i < dirty_inodes.count; i++){
          if(dirty_inodes.inums[i] == entry->inum){
            dirty_idx = i;
            break;
          }
        }
        if(dirty_idx >= 0){
          dirty_inodes.inodes[dirty_idx].addrs[NDIRECT] = new_ind;
        }
        release(&dirty_inodes.lock);

        new_ind_for_icache = new_ind;
      }
    }

    // NOTE: We rely on lock-free optimistic update for icache to avoid panic.
    for(struct inode *ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++){
      if(ip->ref > 0 && ip->dev == lfs.dev && ip->inum == entry->inum){
        if(bn < NDIRECT){
          ip->addrs[bn] = new_block;
        } else if(new_ind_for_icache != 0){
          ip->addrs[NDIRECT] = new_ind_for_icache;
        }
        break;
      }
    }
    return 0;  // Success for dirty buffer path
  }

read_from_disk:
  // Inode not in dirty buffer - need to read, modify, and add to dirty buffer
  acquire(&lfs.lock);
  imap_entry = lfs.imap[entry->inum];
  release(&lfs.lock);

  if(imap_entry == 0 || imap_entry == 0xFFFFFFFF){
    return 0;  // Inode was freed during relocation - this is OK
  }

  inode_block = IMAP_BLOCK(imap_entry);
  inode_slot = IMAP_SLOT(imap_entry);

  // Validate inode_block
  if(inode_block >= sb.size){
    cprintf("gc_relocate_block: INVALID inode_block=%d >= size=%d (inum=%d, imap_entry=0x%x)\n",
            inode_block, sb.size, entry->inum, imap_entry);
    return -1;
  }

  bp_inode = bread(lfs.dev, inode_block);
  dip = (struct dinode*)bp_inode->data + inode_slot;

  // Create a copy of the dinode
  struct dinode di_copy;
  memmove(&di_copy, dip, sizeof(di_copy));
  brelse(bp_inode);

  // Skip if inode is freed (type == 0) - addrs[] may contain garbage
  if(di_copy.type == 0){
    return 0;  // Inode was freed - this is OK
  }

  if(bn < NDIRECT || entry->type == SSB_TYPE_INDIRECT){
    // Direct block or INDIRECT block type: just update addrs directly
    if(entry->type == SSB_TYPE_INDIRECT){
      di_copy.addrs[NDIRECT] = new_block;
    } else {
      di_copy.addrs[bn] = new_block;
    }
  } else {
    // DATA block accessed through indirect - need COW for indirect block
    uint old_ind = di_copy.addrs[NDIRECT];
    if(old_ind != 0){
      struct buf *bp_ind, *bp_new_ind;
      uint new_ind;

      acquire(&lfs.lock);
      if(lfs.log_tail >= lfs.cur_seg_end){
        if(lfs.free_count > 0){
          uint free_seg = lfs.free_segs[lfs.free_head];
          lfs.free_head = (lfs.free_head + 1) % LFS_NSEGS_MAX;
          lfs.free_count--;
          lfs.log_tail = sb.segstart + free_seg * sb.segsize;
          lfs.cur_seg_end = lfs.log_tail + sb.segsize;
          lfs.sut[free_seg].live_bytes = 0;
        } else {
          // Out of space for indirect block - cannot continue
          release(&lfs.lock);
          return -1;
        }
      }
      new_ind = lfs.log_tail++;

      // Atomically add SSB entry for new indirect block
      if(lfs.ssb_count < SSB_ENTRIES_PER_BLOCK){
        lfs.ssb_buf[lfs.ssb_count].type = SSB_TYPE_INDIRECT;
        lfs.ssb_buf[lfs.ssb_count].inum = entry->inum;
        lfs.ssb_buf[lfs.ssb_count].offset = NDIRECT;
        lfs.ssb_buf[lfs.ssb_count].version = current_version;
        lfs.ssb_count++;
      }
      release(&lfs.lock);

      // Validate old_ind before reading
      if(old_ind >= sb.size){
        cprintf("gc_relocate_block(not dirty): INVALID old_ind=%d >= size=%d (inum=%d)\n",
                old_ind, sb.size, entry->inum);
        return -1;
      }

      bp_ind = bread(lfs.dev, old_ind);
      bp_new_ind = bread(lfs.dev, new_ind);
      memmove(bp_new_ind->data, bp_ind->data, BSIZE);
      if(entry->type == SSB_TYPE_DATA){
        uint *a = (uint*)bp_new_ind->data;
        // Validate index
        if(bn < NDIRECT || bn - NDIRECT >= NINDIRECT){
          cprintf("gc_relocate_block: INVALID indirect index bn=%d\n", bn);
          brelse(bp_new_ind);
          brelse(bp_ind);
          return -1;
        }
        a[bn - NDIRECT] = new_block;
      }
      bwrite(bp_new_ind);
      brelse(bp_new_ind);
      brelse(bp_ind);

      lfs_update_usage(new_ind, BSIZE);
      lfs_update_usage(old_ind, -BSIZE);
      di_copy.addrs[NDIRECT] = new_ind;
    }
  }

  // Add modified inode to dirty buffer (or update if already exists)
  acquire(&dirty_inodes.lock);

  // Check if inode already exists in dirty buffer
  int found_slot = -1;
  for(int di = 0; di < dirty_inodes.count; di++){
    if(dirty_inodes.inums[di] == entry->inum){
      found_slot = di;
      break;
    }
  }

  if(found_slot >= 0){
    // Update existing entry
    memmove(&dirty_inodes.inodes[found_slot], &di_copy, sizeof(di_copy));
    dirty_inodes.versions[found_slot] = current_version;
  } else {
    // Add new entry
    if(dirty_inodes.count >= IPB){
      release(&dirty_inodes.lock);
      lfs_sync();
      acquire(&dirty_inodes.lock);
    }
    memmove(&dirty_inodes.inodes[dirty_inodes.count], &di_copy, sizeof(di_copy));
    dirty_inodes.inums[dirty_inodes.count] = entry->inum;
    dirty_inodes.versions[dirty_inodes.count] = current_version;
    dirty_inodes.count++;
  }
  release(&dirty_inodes.lock);

  // NOTE: We rely on lock-free optimistic update for icache to avoid panic.
  for(struct inode *ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == lfs.dev && ip->inum == entry->inum){
      if(bn < NDIRECT){
        ip->addrs[bn] = new_block;
      } else {
        // For indirect, update the indirect block pointer
        ip->addrs[NDIRECT] = di_copy.addrs[NDIRECT];
      }
      break;
    }
  }
  return 0;  // Success
}

// Clean a single segment: relocate live blocks, mark segment as free
// Returns number of live blocks relocated, or -1 if GC had to stop early
static int
gc_clean_segment(uint seg_idx)
{
  uint ssb_addrs[LFS_SEGSIZE];  // Max SSBs in a segment
  int ssb_count;
  int live_blocks = 0;
  int total_blocks = 0;
  int stopped_early = 0;  // Flag for early exit due to out of space

  // Track relocated inode blocks to avoid duplicates
  // (Multiple inodes may share the same block)
  uint relocated_inode_blocks[IPB * 4];  // Track up to IPB*4 unique blocks
  int relocated_count = 0;

  // 1. Find all SSB blocks in this segment
  ssb_count = gc_find_ssbs(seg_idx, ssb_addrs, LFS_SEGSIZE);
  // Even if no SSBs found (e.g., mkfs segments), we continue to the
  // imap safety scan below, which catches inode blocks without SSB entries.

  // 2. Process each SSB and its described blocks
  for(int s = 0; s < ssb_count; s++){
    struct buf *bp = bread(lfs.dev, ssb_addrs[s]);
    struct ssb *ssb_ptr = (struct ssb *)bp->data;

    // Calculate block addresses for entries
    // SSB entries describe blocks written BEFORE the SSB in the segment
    // We need to figure out which blocks correspond to which entries
    // In our implementation, entries are added as blocks are written,
    // so entry[i] corresponds to the i-th data block before this SSB

    // For simplicity, we'll use the offset stored in ssb_entry
    // to identify blocks and check if they're live
    for(int e = 0; e < ssb_ptr->nblocks; e++){
      struct ssb_entry *entry = &ssb_ptr->entries[e];
      total_blocks++;

      if(entry->type == SSB_TYPE_INODE){
        // Inode Block entry. Check if ANY imap entry points to a block in this segment.
        // Simplified: one SSB entry per inode block, no version check needed.
        // Liveness is determined by whether imap points to this block.
        uint seg_start = sb.segstart + seg_idx * sb.segsize;
        uint seg_end = seg_start + sb.segsize;

        acquire(&lfs.lock);
        for(int ino = 1; ino < LFS_NINODES; ino++){
          uint imap_entry = lfs.imap[ino];
          if(imap_entry != 0 && imap_entry != 0xFFFFFFFF){
            uint iblk = IMAP_BLOCK(imap_entry);
            // Check if this imap points to a block in the segment being cleaned
            if(iblk >= seg_start && iblk < seg_end){
              // Block is live. Check if already relocated.
              int already_relocated = 0;
              for(int r = 0; r < relocated_count; r++){
                if(relocated_inode_blocks[r] == iblk){
                  already_relocated = 1;
                  break;
                }
              }
              if(!already_relocated){
                release(&lfs.lock);
                if(gc_relocate_inode_block(ino, iblk) < 0){
                  brelse(bp);
                  cprintf("GC: out of space relocating inode block\n");
                  stopped_early = 1;
                  goto gc_early_exit;
                }
                live_blocks++;
                if(relocated_count < IPB * 4)
                  relocated_inode_blocks[relocated_count++] = iblk;
                acquire(&lfs.lock);
              }
            }
          }
        }
        release(&lfs.lock);
        continue;
      }

      // DATA or INDIRECT block
      // Get imap entry for this inode
      uint imap_entry;
      acquire(&lfs.lock);
      imap_entry = lfs.imap[entry->inum];
      release(&lfs.lock);

      if(imap_entry == 0 || imap_entry == 0xFFFFFFFF){
        continue;  // Inode deleted or in-flight
      }

      // Check version first (fast path)
      if(IMAP_VERSION(imap_entry) != entry->version){
        continue;  // Version mismatch - block is dead
      }

      // Find the actual block address for data block entries
      uint block_addr = 0;
      uint bn = entry->offset;
      int found_in_dirty = 0;
      struct dinode di_copy;

      // IMPORTANT: Check dirty buffer first for latest inode state!
      // This prevents GC from using stale disk data when inode was recently modified.
      acquire(&dirty_inodes.lock);
      for(int di = 0; di < dirty_inodes.count; di++){
        if(dirty_inodes.inums[di] == entry->inum){
          // Found in dirty buffer - use this (latest state)
          memmove(&di_copy, &dirty_inodes.inodes[di], sizeof(di_copy));
          found_in_dirty = 1;
          break;
        }
      }
      // Also check flushing buffer
      if(!found_in_dirty){
        for(int di = 0; di < dirty_inodes.flushing_count; di++){
          if(dirty_inodes.flushing_inums[di] == entry->inum){
            memmove(&di_copy, &dirty_inodes.flushing_inodes[di], sizeof(di_copy));
            found_in_dirty = 1;
            break;
          }
        }
      }
      release(&dirty_inodes.lock);

      if(found_in_dirty){
        // Use inode data from dirty buffer
        if(di_copy.type == 0){
          continue;  // Inode freed
        }
        if(entry->type == SSB_TYPE_INDIRECT){
          block_addr = di_copy.addrs[NDIRECT];
        } else {
          // DATA block
          if(bn < NDIRECT){
            block_addr = di_copy.addrs[bn];
          } else {
            uint ind_addr = di_copy.addrs[NDIRECT];
            if(ind_addr == 0){
              continue;
            }
            if(ind_addr >= sb.size){
              cprintf("gc_clean_segment: INVALID ind_addr=%d (dirty, inum=%d)\n",
                      ind_addr, entry->inum);
              continue;
            }
            if(bn - NDIRECT >= NINDIRECT){
              continue;
            }
            struct buf *bp_ind = bread(lfs.dev, ind_addr);
            uint *a = (uint*)bp_ind->data;
            block_addr = a[bn - NDIRECT];
            brelse(bp_ind);
          }
        }
      } else {
        // Read inode from disk
        uint inode_block = IMAP_BLOCK(imap_entry);
        uint inode_slot = IMAP_SLOT(imap_entry);

        // Validate inode_block before reading
        if(inode_block >= sb.size){
          cprintf("gc_clean_segment: INVALID inode_block=%d >= size=%d (inum=%d)\n",
                  inode_block, sb.size, entry->inum);
          continue;  // Skip this entry
        }

        struct buf *bp_inode = bread(lfs.dev, inode_block);
        struct dinode *dip = (struct dinode*)bp_inode->data + inode_slot;

        // Skip if inode is freed (type == 0) - addrs[] may contain garbage
        if(dip->type == 0){
          brelse(bp_inode);
          continue;
        }

        if(entry->type == SSB_TYPE_INDIRECT){
           // Indirect block is stored at addrs[NDIRECT]
           block_addr = dip->addrs[NDIRECT];
        } else {
           // DATA block
           if(bn < NDIRECT){
             block_addr = dip->addrs[bn];
           } else {
             uint ind_addr = dip->addrs[NDIRECT];
             brelse(bp_inode);
             if(ind_addr == 0){
               continue;
             }
             // Validate ind_addr before reading
             if(ind_addr >= sb.size){
               cprintf("gc_clean_segment: INVALID ind_addr=%d >= size=%d (inum=%d)\n",
                       ind_addr, sb.size, entry->inum);
               continue;  // Skip this entry
             }
             // Validate bn for indirect access
             if(bn < NDIRECT || bn - NDIRECT >= NINDIRECT){
               cprintf("gc_clean_segment: INVALID bn=%d for indirect access\n", bn);
               continue;
             }
             struct buf *bp_ind = bread(lfs.dev, ind_addr);
             uint *a = (uint*)bp_ind->data;
             block_addr = a[bn - NDIRECT];
             brelse(bp_ind);
             bp_inode = 0;  // Mark as released
           }
        }

        if(bp_inode) brelse(bp_inode);
      }

      if(block_addr == 0){
        continue;  // Block not allocated
      }

      // Validate block_addr before using it
      if(block_addr >= sb.size){
        cprintf("gc_clean_segment: INVALID block_addr=%d >= size=%d (inum=%d, type=%d)\n",
                block_addr, sb.size, entry->inum, entry->type);
        continue;  // Skip this entry - corrupted data
      }

      // Check if block is in this segment
      if(block_addr < sb.segstart){
        continue;  // Block not in log area
      }
      uint block_seg = (block_addr - sb.segstart) / sb.segsize;
      if(block_seg != seg_idx){
        continue;  // Block not in this segment (already relocated?)
      }

      // Block is live - relocate it
      if(gc_relocate_block(entry, block_addr) < 0){
        // Out of space - stop cleaning this segment
        brelse(bp);
        cprintf("GC: out of space during relocation, stopping early\n");
        stopped_early = 1;
        goto gc_early_exit;
      }
      live_blocks++;
    }

    brelse(bp);
  }

  // 3. Fallback scan for segments without SSBs (e.g., corrupted or very old segments)
  if(ssb_count == 0){
    uint seg_start = sb.segstart + seg_idx * sb.segsize;
    uint seg_end = seg_start + sb.segsize;

    // Scan imap for inode blocks in this segment
    acquire(&lfs.lock);
    for(int i = 1; i < LFS_NINODES; i++){
      uint entry = lfs.imap[i];
      if(entry != 0 && entry != 0xFFFFFFFF){
        uint blk = IMAP_BLOCK(entry);
        if(blk >= seg_start && blk < seg_end){
          // Check if we already relocated this block
          int already_done = 0;
          for(int r = 0; r < relocated_count; r++){
            if(relocated_inode_blocks[r] == blk){
              already_done = 1;
              break;
            }
          }
          if(!already_done && relocated_count < IPB * 4){
            relocated_inode_blocks[relocated_count++] = blk;
            release(&lfs.lock);
            if(gc_relocate_inode_block(i, blk) < 0){
              cprintf("GC: out of space in fallback inode relocation\n");
              stopped_early = 1;
              goto gc_early_exit;
            }
            live_blocks++;
            acquire(&lfs.lock);
          }
        }
      }
    }
    release(&lfs.lock);

    // For segments with no SSBs, we also need to scan data blocks
    // This is the O(N) fallback for mkfs segments only
    for(int i = 1; i < LFS_NINODES; i++){
      uint imap_entry;
      acquire(&lfs.lock);
      imap_entry = lfs.imap[i];
      release(&lfs.lock);

      if(imap_entry == 0 || imap_entry == 0xFFFFFFFF)
        continue;

      // Read inode
      struct dinode di;
      uint iblk = IMAP_BLOCK(imap_entry);
      uint islot = IMAP_SLOT(imap_entry);

      // Validate iblk before reading
      if(iblk >= sb.size){
        cprintf("GC fallback scan: INVALID iblk=%d (inum=%d)\n", iblk, i);
        continue;
      }

      struct buf *bp_in = bread(lfs.dev, iblk);
      memmove(&di, (struct dinode*)bp_in->data + islot, sizeof(di));
      brelse(bp_in);

      if(di.type == 0) continue;

      // Check direct blocks
      for(int bn = 0; bn < NDIRECT; bn++){
        uint dblk = di.addrs[bn];
        if(dblk != 0 && dblk >= seg_start && dblk < seg_end){
          struct ssb_entry fake_entry;
          fake_entry.type = SSB_TYPE_DATA;
          fake_entry.inum = i;
          fake_entry.offset = bn;
          fake_entry.version = IMAP_VERSION(imap_entry);
          if(gc_relocate_block(&fake_entry, dblk) < 0){
            cprintf("GC: out of space in fallback direct, stopping\n");
            stopped_early = 1;
            goto gc_early_exit;
          }
          live_blocks++;
        }
      }

      // Check indirect block pointer
      uint ind_addr = di.addrs[NDIRECT];
      if(ind_addr != 0 && ind_addr >= seg_start && ind_addr < seg_end){
        // Relocate indirect block itself
        live_blocks++;
        struct buf *bp_old = bread(lfs.dev, ind_addr);
        uint new_ind = lfs_alloc();
        lfs_write_pending_ssb();
        struct buf *bp_new = bread(lfs.dev, new_ind);
        memmove(bp_new->data, bp_old->data, BSIZE);
        bwrite(bp_new);
        brelse(bp_new);
        brelse(bp_old);
        lfs_update_usage(new_ind, BSIZE);
        lfs_update_usage(ind_addr, -BSIZE);

        // Add SSB entry for the relocated indirect block
        lfs_add_ssb_entry(SSB_TYPE_INDIRECT, i, NDIRECT, IMAP_VERSION(imap_entry));

        // Update inode
        acquire(&dirty_inodes.lock);
        int found = 0;
        for(int d = 0; d < dirty_inodes.count; d++){
          if(dirty_inodes.inums[d] == (uint)i){
            dirty_inodes.inodes[d].addrs[NDIRECT] = new_ind;
            found = 1;
            break;
          }
        }
        if(!found && dirty_inodes.count < IPB){
          di.addrs[NDIRECT] = new_ind;
          memmove(&dirty_inodes.inodes[dirty_inodes.count], &di, sizeof(di));
          dirty_inodes.inums[dirty_inodes.count] = i;
          dirty_inodes.versions[dirty_inodes.count] = IMAP_VERSION(imap_entry);
          dirty_inodes.count++;
        }
        release(&dirty_inodes.lock);
        ind_addr = new_ind;
      }

      // Check data blocks in indirect
      if(ind_addr != 0){
        struct buf *bp_ind = bread(lfs.dev, ind_addr);
        uint *ind_data = (uint*)bp_ind->data;
        for(int bn = 0; bn < NINDIRECT; bn++){
          uint dblk = ind_data[bn];
          if(dblk != 0 && dblk >= seg_start && dblk < seg_end){
            struct ssb_entry fake_entry;
            fake_entry.type = SSB_TYPE_DATA;
            fake_entry.inum = i;
            fake_entry.offset = NDIRECT + bn;
            fake_entry.version = IMAP_VERSION(imap_entry);
            brelse(bp_ind);
            if(gc_relocate_block(&fake_entry, dblk) < 0){
              cprintf("GC: out of space in fallback indirect, stopping\n");
              stopped_early = 1;
              goto gc_early_exit;
            }
            live_blocks++;
            // Re-read indirect after relocation
            acquire(&lfs.lock);
            imap_entry = lfs.imap[i];
            release(&lfs.lock);
            if(imap_entry == 0 || imap_entry == 0xFFFFFFFF){
              break;  // Inode was deleted during relocation
            }
            iblk = IMAP_BLOCK(imap_entry);
            islot = IMAP_SLOT(imap_entry);
            if(iblk >= sb.size){
              cprintf("GC fallback: INVALID iblk=%d (inum=%d)\n", iblk, i);
              break;
            }
            bp_in = bread(lfs.dev, iblk);
            ind_addr = ((struct dinode*)bp_in->data + islot)->addrs[NDIRECT];
            brelse(bp_in);
            if(ind_addr == 0) break;
            if(ind_addr >= sb.size){
              cprintf("GC fallback: INVALID ind_addr=%d (inum=%d)\n", ind_addr, i);
              break;
            }
            bp_ind = bread(lfs.dev, ind_addr);
            ind_data = (uint*)bp_ind->data;
          }
        }
        brelse(bp_ind);
      }
    }
  }

gc_early_exit:
  // 4. Flush SSB for all relocated blocks in this cleaning run
  lfs_write_ssb_now();

  // 5. Mark segment as free ONLY if we completed successfully
  //    If stopped_early, there are still live blocks in this segment
  if(!stopped_early){
    gc_free_segment(seg_idx);
  }

  return stopped_early ? -1 : live_blocks;
}

// Main GC function: select and clean victim segments
static void
lfs_gc(void)
{
  struct gc_victim victims[GC_TARGET_SEGS];
  int victim_count;
  int total_cleaned = 0;

  // 1. Set GC running flag
  acquire(&lfs.lock);
  if(lfs.gc_running){
    release(&lfs.lock);
    return;  // GC already in progress
  }
  lfs.gc_running = 1;
  release(&lfs.lock);

  // cprintf("GC: starting garbage collection (free_count=%d)\n", lfs.free_count);

  // 2. Select victim segments using cost-benefit policy
  victim_count = gc_select_victims(victims, GC_TARGET_SEGS);

  if(victim_count == 0){
    // cprintf("GC: no suitable segments to clean\n");
    acquire(&lfs.lock);
    lfs.gc_failed = 1;  // Prevent repeated GC triggers
    lfs.gc_running = 0;
    release(&lfs.lock);
    return;
  }

  // 2.5. Check if we have enough space to perform GC
  //      We need at least half a segment's worth of space to safely relocate blocks
  acquire(&lfs.lock);
  uint remaining_in_current = lfs.cur_seg_end - lfs.log_tail;
  uint min_space_needed = sb.segsize / 2;  // Need at least half a segment
  if(remaining_in_current < min_space_needed && lfs.free_count == 0){
    // Not enough space to safely perform GC - skip
    lfs.gc_failed = 1;
    lfs.gc_running = 0;
    release(&lfs.lock);
    cprintf("GC: not enough space to run GC (remaining=%d, free_count=0)\n", remaining_in_current);
    return;
  }
  release(&lfs.lock);

  // cprintf("GC: %d segments selected for cleaning\n", victim_count);

  // 3. Clean each victim segment (one at a time, freeing as we go)
  int gc_success = 1;
  for(int i = 0; i < victim_count; i++){
    // cprintf("GC: cleaning segment %d, score %d, util %d%%\n",
    //         victims[i].seg_idx, victims[i].score, victims[i].util_percent);
    int result = gc_clean_segment(victims[i].seg_idx);
    if(result < 0){
      // Out of space - stop cleaning
      gc_success = 0;
      break;
    }
    total_cleaned += result;
  }

  // 4. Clear gc_running BEFORE sync so lfs_sync() actually runs
  //    Timer interrupt sync is still blocked by syncing flag inside lfs_sync()
  acquire(&lfs.lock);
  lfs.gc_running = 0;
  release(&lfs.lock);

  // 5. Sync to persist changes
  lfs_sync();

  // Free segments are now added directly in gc_free_segment()
  // cprintf("GC: done, %d free segments available\n", lfs.free_count);
  acquire(&lfs.lock);
  if(gc_success && total_cleaned > 0){
    lfs.gc_failed = 0;  // GC succeeded, allow future triggers
  } else {
    lfs.gc_failed = 1;  // GC failed or couldn't clean anything
  }
  release(&lfs.lock);
}

// ============================================================================
// End of Garbage Collection Implementation
// ============================================================================

// Sync: flush dirty inodes, write imap, write checkpoint
// Called when segment is full, buffer is full, or periodically
void
lfs_sync(void)
{
  // Recursion guard and GC guard
  acquire(&lfs.lock);
  if(lfs.syncing || lfs.gc_running){
    // Skip if already syncing or GC is running (GC will call sync when done)
    release(&lfs.lock);
    return;
  }
  lfs.syncing = 1;
  release(&lfs.lock);

  // Check if there's anything to sync
  acquire(&dirty_inodes.lock);
  int has_dirty = (dirty_inodes.count > 0);
  release(&dirty_inodes.lock);
  
  acquire(&lfs.lock);
  int has_ssb = (lfs.ssb_count > 0);
  release(&lfs.lock);

  if(!has_dirty && !has_ssb){
    acquire(&lfs.lock);
    lfs.syncing = 0;
    release(&lfs.lock);
    return;  // Nothing to sync
  }

  // 1. Flush dirty inodes to disk (updates in-memory imap)
  lfs_flush_inodes();
  
  // 2. Flush SSB (Segment Summary Block)
  // Use lfs_write_ssb_now() which handles out-of-space gracefully
  lfs_write_ssb_now();

  // 3. Write SUT (Segment Usage Table)
  lfs_write_sut();

  // 4. Write imap to log
  lfs_write_imap();

  // 5. Write checkpoint
  lfs_write_checkpoint();

  // Debug: cprintf("LFS sync: log_tail now %d\n", lfs.log_tail);
  
  acquire(&lfs.lock);
  lfs.syncing = 0;
  release(&lfs.lock);
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
  // Use static to avoid potential stack issues
  static uint imap_copy[LFS_NINODES];

  // Copy imap under lock
  acquire(&lfs.lock);
  memmove(imap_copy, lfs.imap, sizeof(imap_copy));
  lfs.cp.imap_nblocks = nblocks;
  release(&lfs.lock);

  for(i = 0; i < nblocks && i < NIMAP_BLOCKS; i++){
    // Allocate block for imap
    acquire(&lfs.lock);
    // Check if we need to switch to a free segment
    if(lfs.log_tail >= lfs.cur_seg_end){
      if(lfs.free_count > 0){
        uint free_seg = lfs.free_segs[lfs.free_head];
        lfs.free_head = (lfs.free_head + 1) % LFS_NSEGS_MAX;
        lfs.free_count--;
        lfs.log_tail = sb.segstart + free_seg * sb.segsize;
        lfs.cur_seg_end = lfs.log_tail + sb.segsize;
        lfs.sut[free_seg].live_bytes = 0;
        // cprintf("LFS: imap switching to free segment %d\n", free_seg);
      } else {
        release(&lfs.lock);
        panic("lfs_write_imap: out of disk space (no free segments)");
      }
    }
    block = lfs.log_tail++;
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

// Allocate a block from the log tail with optional atomic SSB entry
// ssb_type = 0 means no SSB entry (for internal metadata like imap, checkpoint)
// With GC integration: triggers GC when disk is nearly full
static uint
lfs_alloc_with_ssb(uchar ssb_type, uint ssb_inum, uint ssb_offset, uint ssb_version)
{
  uint block;
  int should_sync = 0;
  int should_gc = 0;

  if(holding(&lfs.lock))
    panic("lfs_alloc: recursive lock acquisition");

  // Check if GC should be triggered (before acquiring lock)
  acquire(&lfs.lock);
  
  // If we are low on segments, reset gc_failed to try again 
  // (deletion might have happened since last failure).
  // We check free_count because cur_seg_end wraps around when recycling.
  if(lfs.gc_failed && lfs.free_count < GC_TARGET_SEGS){
    lfs.gc_failed = 0;
  }

  // Trigger GC when running low on free segments
  if(!lfs.gc_running && !lfs.syncing && !lfs.gc_failed && lfs.free_count < GC_TARGET_SEGS){
    if(lfs.cur_seg_end < sb.size){
      // Already using free segments - disk is full, need GC
      should_gc = 1;
    } else {
      uint used_blocks = lfs.log_tail - sb.segstart;
      uint total_blocks = sb.size - sb.segstart;
      uint usage_percent = (used_blocks * 100) / total_blocks;
      if(usage_percent >= GC_THRESHOLD){
        should_gc = 1;
      }
    }
  }

  release(&lfs.lock);

  // Run GC if needed (outside of lock)
  if(should_gc){
    // Flush SSB before GC to ensure all blocks have SSB coverage
    // But only if we're not already in a sync operation (to avoid recursion)
    if(!lfs.syncing){
      lfs_write_ssb_now();
      lfs_write_pending_ssb();
    }
    lfs_gc();
  }

  acquire(&lfs.lock);
  
  // Check segment boundary and reserve space for Metadata (Inodes + SSB)
  // We reserve 2 blocks: 1 for potential inode flush, 1 for SSB.
  uint offset = (lfs.log_tail - sb.segstart) % sb.segsize;
  uint remaining = sb.segsize - offset;
  uint disk_remaining = lfs.cur_seg_end - lfs.log_tail;

  if((remaining <= 5 || disk_remaining <= 5) && !lfs.syncing){
    should_sync = 1;
  }
  release(&lfs.lock);

  if(should_sync){
    lfs_sync();
  }

  acquire(&lfs.lock);

  // If we are not syncing (regular allocation), we must NEVER allocate
  // from the reserved area (last 2 blocks).
  // If we are in the reserved area, skip to the next segment.
  if(!lfs.syncing){
    while((lfs.log_tail - sb.segstart) % sb.segsize != 0){
      uint off = (lfs.log_tail - sb.segstart) % sb.segsize;
      uint rem = sb.segsize - off;
      // Only skip if we are actually in the reserved zone (last 2 blocks)
      if (rem > 2) break;

      // Check if we wrap around or hit end of allocation region
      if(lfs.log_tail >= lfs.cur_seg_end) break;
      lfs.log_tail++;
    }
  }

  // Check remaining space in current segment
  uint seg_offset = (lfs.log_tail - sb.segstart) % sb.segsize;
  uint seg_remaining = sb.segsize - seg_offset;

  // CRITICAL: If only 2 blocks remain, we MUST reserve them.
  // We need at least 1 block for inode flush and 1 for SSB.
  if(seg_remaining <= 2){
    if(lfs.ssb_count > 0 && !lfs.ssb_flushing){
      // Explicitly reserve the current block for SSB only if we have data to write.
      lfs.reserved_ssb_block = lfs.log_tail;
      
      lfs.ssb_flushing = 1;
      lfs.ssb_pending_count = lfs.ssb_count;
      memmove(lfs.ssb_flush_buf, lfs.ssb_buf, lfs.ssb_count * sizeof(struct ssb_entry));
      lfs.ssb_count = 0;
      // Set pending block to reserved one
      lfs.ssb_pending_block = lfs.reserved_ssb_block;
      
      // Consume the block
      lfs.log_tail++;
    }
    // Now seg_remaining becomes low, triggering segment switch below
    seg_remaining = 0;
  }

  // Check if we hit a segment boundary
  if(seg_remaining == 0){
    // We've hit a segment boundary. Check if we can continue in the sequential area
    // or if we need to use a free segment.

    // Align log_tail to next segment boundary (in case SSB consumed the last block)
    uint next_seg_start = ((lfs.log_tail - sb.segstart + sb.segsize - 1) / sb.segsize) * sb.segsize + sb.segstart;

    if(next_seg_start < lfs.cur_seg_end){
      // Still have sequential space - continue to next segment
      lfs.log_tail = next_seg_start;
      lfs.ssb_seg_start = lfs.log_tail;
    } else {
      // Sequential area exhausted - need to use a free segment
      goto use_free_segment;
    }
  }

  // Check if we've exhausted the current allocation region
  if(lfs.log_tail >= lfs.cur_seg_end){
use_free_segment:
    // Current allocation region exhausted - try to use a free segment
    if(lfs.free_count > 0){
      // Get next free segment
      uint free_seg = lfs.free_segs[lfs.free_head];
      lfs.free_head = (lfs.free_head + 1) % LFS_NSEGS_MAX;
      lfs.free_count--;

      // Validate free segment index
      if(free_seg >= sb.nsegs){
        cprintf("lfs_alloc: INVALID free_seg %d >= nsegs %d\n", free_seg, sb.nsegs);
        release(&lfs.lock);
        panic("lfs_alloc: invalid free segment");
      }

      // Set log_tail to start of free segment
      lfs.log_tail = sb.segstart + free_seg * sb.segsize;
      lfs.cur_seg_end = lfs.log_tail + sb.segsize;

      // Validate new log_tail
      if(lfs.log_tail >= sb.size || lfs.cur_seg_end > sb.size){
        cprintf("lfs_alloc: free_seg %d gives invalid log_tail %d or cur_seg_end %d (size=%d)\n",
                free_seg, lfs.log_tail, lfs.cur_seg_end, sb.size);
        release(&lfs.lock);
        panic("lfs_alloc: invalid segment boundaries");
      }

      // Reset live_bytes for reuse (was marked with SUT_FREE_MARKER)
      lfs.sut[free_seg].live_bytes = 0;
      // Update ssb_seg_start for new segment
      lfs.ssb_seg_start = lfs.log_tail;
      // cprintf("LFS: switching to free segment %d (log_tail=%d)\n",
      //         free_seg, lfs.log_tail);
    } else {
      // No free segments - try GC one more time before giving up
      if(!lfs.gc_running && !lfs.gc_failed){
        lfs.gc_failed = 0;  // Reset to force another attempt
        release(&lfs.lock);
        cprintf("lfs_alloc: emergency GC triggered\n");
        lfs_gc();
        acquire(&lfs.lock);
        // Check if GC freed any segments
        if(lfs.free_count > 0){
          uint free_seg = lfs.free_segs[lfs.free_head];
          lfs.free_head = (lfs.free_head + 1) % LFS_NSEGS_MAX;
          lfs.free_count--;
          lfs.log_tail = sb.segstart + free_seg * sb.segsize;
          lfs.cur_seg_end = lfs.log_tail + sb.segsize;
          lfs.sut[free_seg].live_bytes = 0;
          lfs.ssb_seg_start = lfs.log_tail;
          // Continue to allocate
          goto alloc_block;
        }
      }
      release(&lfs.lock);
      panic("lfs_alloc: out of disk space (no free segments after GC)");
    }
  }

alloc_block:
  block = lfs.log_tail++;

  // Validate block number
  if(block >= sb.size){
    cprintf("lfs_alloc: INVALID block %d >= FSSIZE %d (log_tail=%d, cur_seg_end=%d, free_count=%d)\n",
            block, sb.size, lfs.log_tail, lfs.cur_seg_end, lfs.free_count);
    release(&lfs.lock);
    panic("lfs_alloc: allocated invalid block");
  }

  // Atomically add SSB entry if requested (type != 0)
  // This ensures the entry is added BEFORE any SSB flush that could affect this segment
  if(ssb_type != 0 && lfs.ssb_count < SSB_ENTRIES_PER_BLOCK){
    lfs.ssb_buf[lfs.ssb_count].type = ssb_type;
    lfs.ssb_buf[lfs.ssb_count].inum = ssb_inum;
    lfs.ssb_buf[lfs.ssb_count].offset = ssb_offset;
    lfs.ssb_buf[lfs.ssb_count].version = ssb_version;
    lfs.ssb_count++;
  }

  release(&lfs.lock);

  // Debug: cprintf("LFS alloc: block %d\n", block);
  return block;
}

// Legacy wrapper for internal allocations (imap, checkpoint, SSB itself)
static uint
lfs_alloc(void)
{
  return lfs_alloc_with_ssb(0, 0, 0, 0);
}

// Simplified: inode blocks now use a single SSB entry per block
// GC determines liveness by checking if any imap entry points to the block

// Flush dirty inodes to a single block
// Must be called before checkpoint or when buffer is full
static void
lfs_flush_inodes(void)
{
  struct buf *bp;
  struct dinode *dip;
  uint block;
  int i, count;

  // Check if we have space for Inodes + SSB (need at least 2 blocks)
  // If scarce space remains (<= 2 blocks), skip flushing inodes so SSB can take the last spot.
  // This prevents SSB from spilling into the next segment.
  acquire(&lfs.lock);
  uint offset = (lfs.log_tail - sb.segstart) % sb.segsize;
  uint remaining = sb.segsize - offset;
  uint disk_remaining = lfs.cur_seg_end - lfs.log_tail;
  release(&lfs.lock);

  if(remaining <= 2 || disk_remaining <= 2){
    return;
  }

  // 1. Move to flushing buffer
  acquire(&dirty_inodes.lock);
  count = dirty_inodes.count;
  if(count == 0){
    release(&dirty_inodes.lock);
    return;
  }
  
  if(dirty_inodes.flushing_count > 0)
    panic("lfs_flush_inodes: flush already in progress");

  for(i = 0; i < count; i++){
    memmove(&dirty_inodes.flushing_inodes[i], &dirty_inodes.inodes[i], sizeof(struct dinode));
    dirty_inodes.flushing_inums[i] = dirty_inodes.inums[i];
    dirty_inodes.flushing_versions[i] = dirty_inodes.versions[i];
  }
  dirty_inodes.flushing_count = count;
  dirty_inodes.count = 0;
  release(&dirty_inodes.lock);

  // 2. Allocate block with single SSB entry for the inode block
  // Debug: cprintf("LFS flush %d inodes\n", count);
  // Use first inum as identifier; GC will check all imaps pointing to this block
  block = lfs_alloc_with_ssb(SSB_TYPE_INODE, dirty_inodes.flushing_inums[0], 0, 0);
  lfs_write_pending_ssb();  // Write any pending SSB before bread

  // 3. Write inodes to block (using flushing buffer)
  bp = bread(lfs.dev, block);
  memset(bp->data, 0, BSIZE);
  dip = (struct dinode*)bp->data;

  // Safe to read flushing buffer without lock?
  // Yes, because flushing_count > 0 prevents others from overwriting it.
  // And iput only reads it or modifies active buffer.
  for(i = 0; i < count; i++){
    memmove(&dip[i], &dirty_inodes.flushing_inodes[i], sizeof(struct dinode));
  }
  bwrite(bp);
  brelse(bp);

  // 4. Update imap (SSB entries already added atomically in lfs_alloc_for_inode_block)
  acquire(&lfs.lock);
  for(i = 0; i < count; i++){
    uint inum = dirty_inodes.flushing_inums[i];
    uint version = dirty_inodes.flushing_versions[i];
    // Only update if inode is valid (type != 0).
    // Check type, not imap, because new inodes have imap=0 initially.
    if(dirty_inodes.flushing_inodes[i].type != 0) {
      lfs.imap[inum] = IMAP_ENCODE(block, version, i);
    }
  }
  release(&lfs.lock);

  // 5. Clear flushing buffer
  acquire(&dirty_inodes.lock);
  dirty_inodes.flushing_count = 0;
  release(&dirty_inodes.lock);
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
  dirty_inodes.flushing_count = 0;
  lfs.dev = dev;

  // Initialize GC state
  lfs.free_head = 0;
  lfs.free_tail = 0;
  lfs.free_count = 0;
  lfs.gc_running = 0;
  lfs.reserved_ssb_block = 0;

  readsb(dev, &sb);

  // Verify LFS magic
  if(sb.magic != LFS_MAGIC){
    panic("iinit: not an LFS filesystem");
  }

  // Read checkpoint and imap
  lfs_read_checkpoint(dev);
  lfs_read_imap(dev);
  lfs_read_sut(dev);

  // Initialize cur_seg_end: sequential allocation up to end of disk
  lfs.cur_seg_end = sb.size;

  // Initialize ssb_seg_start: segment that current SSB entries belong to
  lfs.ssb_seg_start = sb.segstart + ((lfs.log_tail - sb.segstart) / sb.segsize) * sb.segsize;

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
      // Debug: cprintf("ialloc: inum %d type %d\n", inum, type);

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
      dirty_inodes.versions[dirty_inodes.count] = 0; // Initialize version to 0
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
      dirty_inodes.versions[i] = ip->version; // Update version
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
    dirty_inodes.versions[dirty_inodes.count] = ip->version; // Store version
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
    
    // Check active buffer
    for(i = 0; i < dirty_inodes.count; i++){
      if(dirty_inodes.inums[i] == ip->inum){
        // Found in dirty buffer - copy from there
        dip = &dirty_inodes.inodes[i];
        ip->type = dip->type;
        ip->major = dip->major;
        ip->minor = dip->minor;
        ip->nlink = dip->nlink;
        ip->size = dip->size;
        ip->version = dirty_inodes.versions[i]; 
        memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
        found = 1;
        break;
      }
    }
    
    // Check flushing buffer if not found
    if(!found){
      for(i = 0; i < dirty_inodes.flushing_count; i++){
        if(dirty_inodes.flushing_inums[i] == ip->inum){
          // Found in flushing buffer
          dip = &dirty_inodes.flushing_inodes[i];
          ip->type = dip->type;
          ip->major = dip->major;
          ip->minor = dip->minor;
          ip->nlink = dip->nlink;
          ip->size = dip->size;
          ip->version = dirty_inodes.flushing_versions[i];
          memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
          found = 1;
          break;
        }
      }
    }
    
    release(&dirty_inodes.lock);

    if(!found){
      // Look up inode location in imap
      acquire(&lfs.lock);
      imap_entry = lfs.imap[ip->inum];
      release(&lfs.lock);

      if(imap_entry == 0){
        // Newly allocated inode (from ialloc) might not be in imap yet if not flushed?
        // But ialloc puts it in dirty buffer. 
        // So this case is only for truly empty inodes?
        // But ilock is called on allocated inodes.
        // Except if iget called on non-existent inode?
        // Let's assume it's valid if we are here, or it's a fresh inode (type 0).
        // If type is 0, valid=1 is fine.
        // But we panic below if type==0.
        // Actually ialloc calls iget, then returns. Caller locks it?
        // No, ialloc returns unlocked. Caller locks.
        // If ialloc put it in dirty buffer, we found it above.
        // If flushed, we find in imap.
        // If imap_entry is 0, it means not allocated.
        cprintf("ilock: inum %d not in imap\n", ip->inum);
        panic("ilock: inode not in imap");
      }

      // Check for placeholder (inode should be in dirty buffer but wasn't found)
      if(imap_entry == 0xFFFFFFFF)
        panic("ilock: inode marked in-flight but not in dirty buffer");

      // Decode block address and slot from imap entry
      block = IMAP_BLOCK(imap_entry);
      slot = IMAP_SLOT(imap_entry);
      ip->version = IMAP_VERSION(imap_entry); // Load version from imap

      // Validate block before reading
      if(block >= sb.size){
        cprintf("ilock: INVALID block=%d >= size=%d (inum=%d, imap_entry=0x%x)\n",
                block, sb.size, ip->inum, imap_entry);
        panic("ilock: corrupted imap entry");
      }

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
            dirty_inodes.versions[i] = dirty_inodes.versions[i+1];
          }
          dirty_inodes.count--;
          break;
        }
      }
      // If it's in flushing buffer, we can't remove it (it's being written). 
      // But we will set imap=0 below, so lfs_flush_inodes will skip updating imap.
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
      // Allocate with atomic SSB entry
      ip->addrs[bn] = addr = lfs_alloc_with_ssb(SSB_TYPE_DATA, ip->inum, bn, ip->version);
      lfs_write_pending_ssb();  // Write any pending SSB before bread
      lfs_update_usage(addr, BSIZE); // New block is live

      // Zero out the new block
      bp = bread(ip->dev, addr);
      memset(bp->data, 0, BSIZE);
      bwrite(bp);
      brelse(bp);
    }
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    // Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0){
      // Allocate indirect block with atomic SSB entry
      ip->addrs[NDIRECT] = addr = lfs_alloc_with_ssb(SSB_TYPE_INDIRECT, ip->inum, NDIRECT, ip->version);
      lfs_write_pending_ssb();  // Write any pending SSB before bread
      lfs_update_usage(addr, BSIZE); // Indirect block live

      // Zero out the new indirect block
      bp = bread(ip->dev, addr);
      memset(bp->data, 0, BSIZE);
      bwrite(bp);
      brelse(bp);
    }
    // Validate indirect block address before reading
    if(addr >= sb.size){
      cprintf("bmap: INVALID indirect addr=%d >= size=%d (inum=%d)\n",
              addr, sb.size, ip->inum);
      panic("bmap: corrupted indirect block address");
    }
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      // Allocate data block with atomic SSB entry
      a[bn] = addr = lfs_alloc_with_ssb(SSB_TYPE_DATA, ip->inum, bn + NDIRECT, ip->version);
      lfs_write_pending_ssb();  // Write any pending SSB before bread
      lfs_update_usage(addr, BSIZE); // Data block live

      // Zero out the new data block
      struct buf *bp_data = bread(ip->dev, addr);
      memset(bp_data->data, 0, BSIZE);
      bwrite(bp_data);
      brelse(bp_data);

      bwrite(bp); // Update indirect block pointer
    }
    brelse(bp);
    return addr;
  }

  panic("bmap: out of range");
}

// Truncate inode (discard contents).
// In LFS with GC, we mark blocks as dead in SUT.
static void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp;
  uint *a;

  // Clear direct blocks
  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      lfs_update_usage(ip->addrs[i], -BSIZE);
      ip->addrs[i] = 0;
    }
  }

  // Clear indirect block
  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j])
        lfs_update_usage(a[j], -BSIZE);
    }
    brelse(bp);
    lfs_update_usage(ip->addrs[NDIRECT], -BSIZE);
    ip->addrs[NDIRECT] = 0;
  }

  ip->size = 0;
  // Increment version on truncate/delete
  ip->version++;
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
    uint addr = bmap(ip, off/BSIZE);
    if(addr >= sb.size){
      cprintf("readi: INVALID bmap addr=%d >= size=%d (inum=%d, off=%d)\n",
              addr, sb.size, ip->inum, off);
      return -1;
    }
    bp = bread(ip->dev, addr);
    m = min(n - tot, BSIZE - off%BSIZE);
    memmove(dst, bp->data + off%BSIZE, m);
    brelse(bp);
  }
  return n;
}

// PAGEBREAK!
// Write data to inode.
// Caller must hold ip->lock.
// In LFS, data blocks are written to the log (Copy-on-Write).
// SSB is written after each batch of data blocks to ensure coverage.
int
writei(struct inode *ip, char *src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;
  uint bn, old_addr, new_addr;
  struct buf *bp_ind, *bp_new_ind;
  uint *a;

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
    bn = off / BSIZE;
    m = min(n - tot, BSIZE - off%BSIZE);

    // 0. Check if SSB needs to be flushed before allocation
    //    This ensures SSB is written to the same segment as data
    if(lfs_prepare_alloc()){
      lfs_write_pending_ssb();
    }

    // 1. Determine old block address
    old_addr = 0;
    if(bn < NDIRECT){
      old_addr = ip->addrs[bn];
    } else {
      uint ind_bn = bn - NDIRECT;
      if(ip->addrs[NDIRECT] != 0){
        // Validate indirect block address before reading
        if(ip->addrs[NDIRECT] >= sb.size){
          cprintf("writei: INVALID indirect addr=%d >= size=%d (inum=%d)\n",
                  ip->addrs[NDIRECT], sb.size, ip->inum);
          return -1;
        }
        bp_ind = bread(ip->dev, ip->addrs[NDIRECT]);
        a = (uint*)bp_ind->data;
        old_addr = a[ind_bn];
        brelse(bp_ind);
      }
    }

    // 2. Allocate NEW block with atomic SSB entry
    new_addr = lfs_alloc_with_ssb(SSB_TYPE_DATA, ip->inum, bn, ip->version);
    lfs_update_usage(new_addr, BSIZE); // New block is live

    // Write any pending SSB from segment switch inside lfs_alloc
    lfs_write_pending_ssb();

    // 3. Copy data / Write new data
    bp = bread(ip->dev, new_addr);
    if(m < BSIZE && old_addr != 0){
      // Partial write: Read old data - validate old_addr first
      if(old_addr >= sb.size){
        brelse(bp);
        cprintf("writei: INVALID old_addr=%d >= size=%d (inum=%d)\n",
                old_addr, sb.size, ip->inum);
        return -1;
      }
      struct buf *bp_old = bread(ip->dev, old_addr);
      memmove(bp->data, bp_old->data, BSIZE);
      brelse(bp_old);
    } else if (m < BSIZE && old_addr == 0) {
      memset(bp->data, 0, BSIZE);
    }
    memmove(bp->data + off%BSIZE, src, m);
    bwrite(bp);
    brelse(bp);

    // 4. Update Inode / Indirect Block (Recursive COW for Indirect)
    if(bn < NDIRECT){
      ip->addrs[bn] = new_addr;
    } else {
      uint ind_bn = bn - NDIRECT;
      uint old_ind = ip->addrs[NDIRECT];
      uint new_ind;

      if(old_ind == 0){
         // New indirect block (with atomic SSB entry)
         new_ind = lfs_alloc_with_ssb(SSB_TYPE_INDIRECT, ip->inum, NDIRECT, ip->version);
         lfs_update_usage(new_ind, BSIZE);
         bp_ind = bread(ip->dev, new_ind);
         memset(bp_ind->data, 0, BSIZE);
      } else {
         // Copy old indirect block to new location (with atomic SSB entry)
         // Validate old_ind before reading
         if(old_ind >= sb.size){
           cprintf("writei: INVALID old_ind=%d >= size=%d (inum=%d)\n",
                   old_ind, sb.size, ip->inum);
           return -1;
         }
         new_ind = lfs_alloc_with_ssb(SSB_TYPE_INDIRECT, ip->inum, NDIRECT, ip->version);
         lfs_update_usage(new_ind, BSIZE);
         lfs_update_usage(old_ind, -BSIZE); // Old indirect dies

         bp_ind = bread(ip->dev, old_ind);
         bp_new_ind = bread(ip->dev, new_ind);
         memmove(bp_new_ind->data, bp_ind->data, BSIZE);
         brelse(bp_ind);
         bp_ind = bp_new_ind;
      }

      a = (uint*)bp_ind->data;
      a[ind_bn] = new_addr;
      bwrite(bp_ind);
      brelse(bp_ind);

      ip->addrs[NDIRECT] = new_ind;
    }

    // 5. Update SUT (SSB entry already added atomically in lfs_alloc_with_ssb)
    if(old_addr != 0){
      lfs_update_usage(old_addr, -BSIZE);
    }
  }

  if(n > 0 && off > ip->size){
    ip->size = off;
  }

  // Update inode in log
  iupdate(ip);

  // SSB entries have been added during block allocation.
  // SSB will be written at segment boundary via lfs_alloc() or during lfs_sync().

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