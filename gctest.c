// gctest.c - Test program for LFS Garbage Collection
// Usage: gctest [mode]
//   mode 1: Fill disk with files, then delete half (create fragmentation)
//   mode 2: Continuous write/delete cycle to trigger GC

#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

#define FILESIZE 32768   // 32KB per file (32 blocks)
#define NFILES   40      // Number of test files

char buf[512];

void
fillbuf(void)
{
  int i;
  for(i = 0; i < sizeof(buf); i++)
    buf[i] = 'A' + (i % 26);
}

// Create a file with given size
int
createfile(char *name, int size)
{
  int fd, i;

  fd = open(name, O_CREATE | O_WRONLY);
  if(fd < 0){
    printf(1, "gctest: cannot create %s\n", name);
    return -1;
  }

  for(i = 0; i < size; i += sizeof(buf)){
    if(write(fd, buf, sizeof(buf)) != sizeof(buf)){
      printf(1, "gctest: write error on %s\n", name);
      close(fd);
      return -1;
    }
  }

  close(fd);
  return 0;
}

// Mode 1: Create files, delete half to create fragmentation
void
test_fragmentation(void)
{
  char name[16];
  int i;

  printf(1, "=== GC Test Mode 1: Fragmentation Test ===\n");

  // Phase 1: Create many files
  printf(1, "Phase 1: Creating %d files (%d bytes each)...\n", NFILES, FILESIZE);
  for(i = 0; i < NFILES; i++){
    name[0] = 'f';
    name[1] = '0' + (i / 10);
    name[2] = '0' + (i % 10);
    name[3] = '\0';

    if(createfile(name, FILESIZE) < 0){
      printf(1, "Failed at file %d\n", i);
      break;
    }

    if((i + 1) % 20 == 0)
      printf(1, "  Created %d files\n", i + 1);
  }
  printf(1, "  Created total %d files\n", i);

  // Phase 2: Delete every other file (create fragmentation)
  printf(1, "Phase 2: Deleting every other file (creating dead blocks)...\n");
  for(i = 0; i < NFILES; i += 2){
    name[0] = 'f';
    name[1] = '0' + (i / 10);
    name[2] = '0' + (i % 10);
    name[3] = '\0';

    if(unlink(name) < 0){
      // File might not exist if creation failed
      continue;
    }
  }
  printf(1, "  Deleted %d files\n", NFILES / 2);

  // Phase 3: Create more files to trigger GC
  printf(1, "Phase 3: Creating more files to trigger GC...\n");
  for(i = 0; i < NFILES / 2; i++){
    name[0] = 'g';
    name[1] = '0' + (i / 10);
    name[2] = '0' + (i % 10);
    name[3] = '\0';

    if(createfile(name, FILESIZE) < 0){
      printf(1, "  Stopped at file %d (possibly disk full or GC triggered)\n", i);
      break;
    }

    if((i + 1) % 20 == 0)
      printf(1, "  Created %d more files\n", i + 1);
  }

  printf(1, "Test complete. Check kernel output for GC messages.\n");
}

// Mode 2: Continuous create/delete cycle
void
test_continuous(void)
{
  char name[16];
  int round, i;

  printf(1, "=== GC Test Mode 2: Continuous Cycle Test ===\n");
  printf(1, "This will run 10 rounds of create/delete cycles.\n");
  printf(1, "Watch kernel output for GC triggers.\n\n");

  for(round = 0; round < 10; round++){
    printf(1, "Round %d: Creating 20 files...\n", round + 1);

    // Create 20 files
    for(i = 0; i < 20; i++){
      name[0] = 't';
      name[1] = '0' + round;
      name[2] = '0' + (i / 10);
      name[3] = '0' + (i % 10);
      name[4] = '\0';

      if(createfile(name, FILESIZE) < 0){
        printf(1, "  Write failed - disk may be full\n");
        goto cleanup;
      }
    }

    // Delete all files from this round
    printf(1, "Round %d: Deleting 20 files...\n", round + 1);
    for(i = 0; i < 20; i++){
      name[0] = 't';
      name[1] = '0' + round;
      name[2] = '0' + (i / 10);
      name[3] = '0' + (i % 10);
      name[4] = '\0';

      unlink(name);
    }
  }

cleanup:
  printf(1, "Continuous test complete.\n");
}

// Mode 3: Fill disk to near capacity
void
test_fill_disk(void)
{
  char name[16];
  int i;
  int large_size = 256 * 1024;  // 256KB per file (fits within MAXFILE)

  printf(1, "=== GC Test Mode 3: Fill Disk Test ===\n");
  printf(1, "Creating large files (%d KB each) to fill disk...\n", large_size / 1024);
  printf(1, "GC should trigger at 50%% disk usage.\n\n");

  for(i = 0; i < 100; i++){  // 100 files * 256KB = 25MB (50% of 50MB disk)
    name[0] = 'x';
    name[1] = '0' + ((i / 10) % 10);
    name[2] = '0' + (i % 10);
    name[3] = '\0';

    if(createfile(name, large_size) < 0){
      printf(1, "\nFailed at file %d - disk full or error\n", i);
      break;
    }

    if((i + 1) % 10 == 0)
      printf(1, "  Created %d files (%d KB total)\n", i + 1, (i + 1) * large_size / 1024);
  }

  printf(1, "\nFill test complete. Created %d files.\n", i);
}

int
main(int argc, char *argv[])
{
  int mode = 1;

  fillbuf();

  if(argc > 1){
    mode = atoi(argv[1]);
  }

  printf(1, "\n========================================\n");
  printf(1, "LFS Garbage Collection Test\n");
  printf(1, "========================================\n\n");

  switch(mode){
  case 1:
    test_fragmentation();
    break;
  case 2:
    test_continuous();
    break;
  case 3:
    test_fill_disk();
    break;
  default:
    printf(1, "Usage: gctest [mode]\n");
    printf(1, "  1 - Fragmentation test (default)\n");
    printf(1, "  2 - Continuous create/delete cycle\n");
    printf(1, "  3 - Fill disk to trigger GC\n");
    break;
  }

  printf(1, "\n========================================\n");
  printf(1, "Check kernel console for GC output:\n");
  printf(1, "  'GC: starting garbage collection'\n");
  printf(1, "  'GC: segment X cleaned, Y live / Z total'\n");
  printf(1, "  'GC: done, N free segments available'\n");
  printf(1, "========================================\n");

  exit();
}
