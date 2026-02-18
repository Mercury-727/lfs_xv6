// Crash test for LFS roll-forward recovery
// Usage:
//   crashtest write   - Create files without checkpoint, then halt (simulates crash)
//   crashtest verify  - Verify files exist after reboot (tests recovery)

#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

#define CRASH_MARKER_FILE "crash_marker"
#define TEST_FILE1 "testfile1"
#define TEST_FILE2 "testfile2"
#define TEST_FILE3 "testfile3"

// Write test data to a file
void
write_test_file(char *name, char *content)
{
  int fd = open(name, O_CREATE | O_WRONLY);
  if(fd < 0){
    printf(1, "crashtest: cannot create %s\n", name);
    exit();
  }
  write(fd, content, strlen(content));
  close(fd);
  printf(1, "crashtest: wrote '%s' to %s\n", content, name);
}

// Read and verify file content
int
verify_file(char *name, char *expected)
{
  char buf[512];
  int fd, n;

  fd = open(name, O_RDONLY);
  if(fd < 0){
    printf(1, "crashtest: FAIL - cannot open %s\n", name);
    return 0;
  }

  n = read(fd, buf, sizeof(buf) - 1);
  close(fd);

  if(n < 0){
    printf(1, "crashtest: FAIL - cannot read %s\n", name);
    return 0;
  }

  buf[n] = '\0';

  if(strcmp(buf, expected) == 0){
    printf(1, "crashtest: PASS - %s contains '%s'\n", name, expected);
    return 1;
  } else {
    printf(1, "crashtest: FAIL - %s contains '%s', expected '%s'\n", name, buf, expected);
    return 0;
  }
}

// Force flush dirty inodes without checkpoint
// This writes data to log but doesn't update checkpoint
void
force_flush_no_checkpoint(void)
{
  // Create enough files to trigger inode buffer flush
  // IPB = 16, so creating 16+ inodes will flush the buffer
  char name[16];
  int i;

  printf(1, "crashtest: creating dummy files to force inode flush...\n");
  for(i = 0; i < 20; i++){
    name[0] = 'd';
    name[1] = 'u';
    name[2] = 'm';
    name[3] = 'm';
    name[4] = 'y';
    name[5] = '0' + (i / 10);
    name[6] = '0' + (i % 10);
    name[7] = '\0';

    int fd = open(name, O_CREATE | O_WRONLY);
    if(fd >= 0){
      write(fd, "x", 1);
      close(fd);
    }
  }
  printf(1, "crashtest: dummy files created, inodes should be flushed to log\n");
}

void
write_mode(void)
{
  printf(1, "\n=== CRASH TEST: WRITE MODE ===\n\n");

  // Step 1: Create a marker file and sync it (this will be in checkpoint)
  printf(1, "Step 1: Creating marker file (will be checkpointed)...\n");
  write_test_file(CRASH_MARKER_FILE, "CHECKPOINT_OK");

  // Force sync to create checkpoint
  // Note: In xv6 LFS, there's no explicit sync syscall,
  // so we rely on periodic sync or buffer full
  printf(1, "Step 2: Waiting for periodic sync (30 sec) or forcing flush...\n");

  // Step 2: Create test files AFTER checkpoint
  // These should be recovered by roll-forward
  printf(1, "Step 3: Creating test files (should be recovered by roll-forward)...\n");
  write_test_file(TEST_FILE1, "RECOVER_ME_1");
  write_test_file(TEST_FILE2, "RECOVER_ME_2");
  write_test_file(TEST_FILE3, "RECOVER_ME_3");

  // Step 3: Force inode flush to write to log (but NOT checkpoint)
  force_flush_no_checkpoint();

  printf(1, "\n");
  printf(1, "=== DATA WRITTEN TO LOG ===\n");
  printf(1, "Now CRASH the system by pressing Ctrl+A then X (QEMU quit)\n");
  printf(1, "Or wait... the system will halt in 5 seconds.\n");
  printf(1, "\n");

  // Wait a bit then halt (simulating crash before checkpoint)
  sleep(500);  // 5 seconds (100 ticks = 1 sec)

  // Halt the system - this is a hard stop without clean shutdown
  // This simulates a power failure / crash
  printf(1, "HALTING SYSTEM (simulating crash)...\n");

  // Use an invalid syscall or infinite loop to crash
  // Or we can just exit and let user kill QEMU manually
  printf(1, "\nPress Ctrl+A, X to quit QEMU now!\n");
  printf(1, "Then run: make qemu-nox\n");
  printf(1, "Then run: crashtest verify\n");

  // Infinite loop to prevent clean exit
  while(1){
    sleep(100);
  }
}

void
verify_mode(void)
{
  int pass = 0, fail = 0;

  printf(1, "\n=== CRASH TEST: VERIFY MODE ===\n\n");

  printf(1, "Checking if crash marker exists (should exist from checkpoint)...\n");
  if(verify_file(CRASH_MARKER_FILE, "CHECKPOINT_OK")){
    pass++;
  } else {
    fail++;
  }

  printf(1, "\nChecking if test files were recovered by roll-forward...\n");

  if(verify_file(TEST_FILE1, "RECOVER_ME_1")){
    pass++;
  } else {
    fail++;
  }

  if(verify_file(TEST_FILE2, "RECOVER_ME_2")){
    pass++;
  } else {
    fail++;
  }

  if(verify_file(TEST_FILE3, "RECOVER_ME_3")){
    pass++;
  } else {
    fail++;
  }

  printf(1, "\n=== RESULTS ===\n");
  printf(1, "PASSED: %d\n", pass);
  printf(1, "FAILED: %d\n", fail);

  if(fail == 0){
    printf(1, "\nROLL-FORWARD RECOVERY SUCCESSFUL!\n");
  } else {
    printf(1, "\nROLL-FORWARD RECOVERY INCOMPLETE.\n");
    printf(1, "Some files written after checkpoint were not recovered.\n");
  }
}

void
cleanup(void)
{
  printf(1, "Cleaning up test files...\n");
  unlink(CRASH_MARKER_FILE);
  unlink(TEST_FILE1);
  unlink(TEST_FILE2);
  unlink(TEST_FILE3);

  // Clean dummy files
  char name[16];
  int i;
  for(i = 0; i < 20; i++){
    name[0] = 'd';
    name[1] = 'u';
    name[2] = 'm';
    name[3] = 'm';
    name[4] = 'y';
    name[5] = '0' + (i / 10);
    name[6] = '0' + (i % 10);
    name[7] = '\0';
    unlink(name);
  }
  printf(1, "Cleanup done.\n");
}

int
main(int argc, char *argv[])
{
  if(argc < 2){
    printf(1, "Usage: crashtest <write|verify|clean>\n");
    printf(1, "\n");
    printf(1, "  write  - Create test files and simulate crash\n");
    printf(1, "  verify - Verify files after reboot (test roll-forward)\n");
    printf(1, "  clean  - Remove test files\n");
    exit();
  }

  if(strcmp(argv[1], "write") == 0){
    write_mode();
  } else if(strcmp(argv[1], "verify") == 0){
    verify_mode();
  } else if(strcmp(argv[1], "clean") == 0){
    cleanup();
  } else {
    printf(1, "Unknown command: %s\n", argv[1]);
  }

  exit();
}
