// LFS Log Layer
//
// In LFS (Log-structured File System), all writes go directly to the log.
// The traditional WAL (Write-Ahead Logging) is not needed because:
// - All data is written sequentially to the log
// - Imap tracks inode locations
// - Checkpoint provides consistency point
//
// The begin_op/end_op/log_write functions are kept as no-ops
// for compatibility with existing code that calls them.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// Minimal log structure for compatibility
struct log {
  struct spinlock lock;
  int outstanding;
};

struct log log;

void
initlog(int dev)
{
  initlock(&log.lock, "log");
  log.outstanding = 0;
  // In LFS, no log recovery needed - checkpoint handles this
}

// Called at the start of each FS system call.
// In LFS, this is just for tracking outstanding operations.
void
begin_op(void)
{
  acquire(&log.lock);
  log.outstanding++;
  release(&log.lock);
}

// Called at the end of each FS system call.
// In LFS, checkpoint is written by iupdate, so this is minimal.
void
end_op(void)
{
  acquire(&log.lock);
  log.outstanding--;
  release(&log.lock);
}

// In LFS, log_write is a no-op.
// All writes go directly to the log via bwrite in fs.c.
void
log_write(struct buf *b)
{
  // No-op in LFS - writes are handled directly
  // The buffer is already written via bwrite()
}
