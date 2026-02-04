#define NPROC        64  // maximum number of processes
#define KSTACKSIZE 4096  // size of per-process kernel stack
#define NCPU          8  // maximum number of CPUs
#define NOFILE       16  // open files per process
#define NFILE       100  // open files per system
#define NINODE       50  // maximum number of active i-nodes
#define NDEV         10  // maximum major device number
#define ROOTDEV       1  // device number of file system root disk
#define MAXARG       32  // max exec arguments
#define MAXOPBLOCKS  10  // max # of blocks any FS op writes
#define LOGSIZE      (MAXOPBLOCKS*3)  // max data blocks in on-disk log
#define NBUF         (MAXOPBLOCKS*3)  // size of disk block cache
#define FSSIZE       20000  // size of file system in blocks (increased for LFS overhead)

// LFS parameters
#define LFS_NINODES   200  // maximum number of inodes in LFS
#define LFS_SEGSIZE   32   // segment size in blocks (reduced to fit SSB)
#define LFS_SEGSTART  4    // first segment starts at block 4 (after boot, sb, cp0, cp1)

// GC parameters
#define GC_THRESHOLD      30   // GC trigger threshold (disk usage %) - trigger early
#define GC_TARGET_SEGS    8    // Number of segments to clean per GC run
#define GC_UTIL_THRESHOLD 95   // Max utilization to consider for cleaning (%)

