# xv6 Sprite LFS 구현 보고서

## 1. 개요

### 1.1 프로젝트 목표
xv6 운영체제에 Sprite LFS (Log-structured File System)를 구현한다.

### 1.2 설계 제약사항
- **GC (Garbage Collection) 미구현**: 로그가 디스크 끝에 도달하면 더 이상 쓰기 불가
- **Crash-Safety 미구현**: 단일 checkpoint 사용 (교대 사용 안 함)
- **기존 inode 구조체 유지**: xv6의 `struct dinode` 그대로 사용

### 1.3 핵심 설계 결정
- **Segment 단위 로그 관리**: 64 블록 단위 세그먼트
- **Imap 로그 방식**: imap도 로그에 순차 기록 (checkpoint만 고정 위치)
- **매 쓰기마다 checkpoint 갱신**: 안정성 우선

---

## 2. 디스크 레이아웃

### 2.1 기존 xv6 레이아웃
```
[ boot | super | log | inode blocks | bitmap | data blocks ]
```

### 2.2 LFS 레이아웃
```
[ boot | super | checkpoint0 | checkpoint1 | log (segments)... ]
   0      1          2             3           4 ~ FSSIZE-1
```

### 2.3 블록 배치 (FSSIZE = 50000)
| 영역 | 블록 번호 | 크기 | 설명 |
|------|----------|------|------|
| Boot | 0 | 1 | 부트 로더 |
| Superblock | 1 | 1 | 파일 시스템 메타데이터 |
| Checkpoint 0 | 2 | 1 | 현재 checkpoint |
| Checkpoint 1 | 3 | 1 | 예비 (미사용) |
| Log | 4-49999 | 49996 | 세그먼트들 (781개) |

---

## 3. 데이터 구조

### 3.1 Superblock (fs.h)
```c
struct superblock {
  uint magic;         // LFS 매직 넘버 (0x4C465321 = "LFS!")
  uint size;          // 파일 시스템 크기 (블록)
  uint nsegs;         // 세그먼트 개수
  uint segsize;       // 세그먼트 크기 (블록)
  uint segstart;      // 첫 세그먼트 시작 블록
  uint ninodes;       // 최대 inode 수
  uint checkpoint0;   // checkpoint 블록 0
  uint checkpoint1;   // checkpoint 블록 1
};
```

### 3.2 Checkpoint (fs.h)
```c
struct checkpoint {
  uint timestamp;                    // checkpoint 시간
  uint log_tail;                     // 현재 로그 끝 위치
  uint cur_seg;                      // 현재 세그먼트 번호
  uint seg_offset;                   // 세그먼트 내 오프셋
  uint imap_addrs[NIMAP_BLOCKS];     // imap 블록들의 디스크 위치
  uint imap_nblocks;                 // 사용 중인 imap 블록 수
  uint valid;                        // 유효성 플래그
};
```

### 3.3 Imap
- **구조**: `imap[inum] = disk_block_address`
- **크기**: 200개 inode 지원 (LFS_NINODES)
- **블록당 엔트리**: 128개 (BSIZE / sizeof(uint))
- **총 imap 블록 수**: 2개

### 3.4 LFS 런타임 상태 (fs.c)
```c
struct {
  struct spinlock lock;
  uint imap[LFS_NINODES];      // inode 번호 -> 디스크 블록 주소
  struct checkpoint cp;        // 현재 checkpoint
  uint log_tail;               // 다음 쓰기 위치
  int dev;                     // 장치 번호
} lfs;
```

---

## 4. 수정된 파일 목록

### 4.1 fs.h
- `struct superblock` 재정의 (LFS용)
- `struct checkpoint` 추가
- `LFS_MAGIC`, `NIMAP_BLOCKS`, `IMAP_ENTRIES_PER_BLOCK` 상수 추가
- `IBLOCK` 매크로 제거 (LFS에서는 inode 위치가 고정되지 않음)

### 4.2 param.h
```c
#define FSSIZE       50000  // 파일 시스템 크기 (블록)
#define LFS_NINODES   200   // 최대 inode 수
#define LFS_SEGSIZE   64    // 세그먼트 크기 (블록)
#define LFS_SEGSTART  4     // 로그 시작 블록
```

### 4.3 fs.c (핵심 변경)

#### 제거된 함수
| 함수 | 이유 |
|------|------|
| `balloc()` | 비트맵 기반 할당 → `lfs_alloc()` 대체 |
| `bfree()` | GC 미구현으로 불필요 |

#### 추가된 함수
| 함수 | 역할 |
|------|------|
| `lfs_read_checkpoint()` | 디스크에서 checkpoint 읽기 |
| `lfs_read_imap()` | checkpoint의 imap 위치에서 imap 로드 |
| `lfs_write_checkpoint()` | checkpoint를 고정 위치에 기록 |
| `lfs_write_imap()` | imap 블록들을 로그에 기록 |
| `lfs_alloc()` | 로그 tail에서 블록 할당 |

#### 수정된 함수
| 함수 | 변경 내용 |
|------|----------|
| `iinit()` | LFS 초기화 (checkpoint, imap 로드) |
| `ialloc()` | imap에 새 엔트리 추가, 로그에 inode 기록 |
| `iupdate()` | inode를 로그의 새 위치에 기록, imap/checkpoint 갱신 |
| `ilock()` | imap 조회하여 inode 디스크 위치 찾기 |
| `iput()` | inode 해제 시 imap에서 제거 |
| `bmap()` | 블록 할당 시 `lfs_alloc()` 사용 |
| `writei()` | 데이터 기록 후 `iupdate()` 호출 |
| `itrunc()` | 블록 해제 없이 참조만 제거 (GC 없음) |

### 4.4 log.c
기존 WAL (Write-Ahead Logging) 제거, 호환성을 위한 no-op 함수 유지:
```c
void begin_op(void)  { /* no-op */ }
void end_op(void)    { /* no-op */ }
void log_write(struct buf *b) { /* no-op */ }
```

### 4.5 mkfs.c
LFS 디스크 이미지 생성:
- Superblock 초기화 (LFS 매직 넘버 포함)
- 초기 checkpoint 생성
- Root 디렉토리 inode를 로그에 기록
- 초기 imap 생성 및 기록
- 사용자 프로그램들을 로그에 기록

### 4.6 Makefile
GCC 최신 버전 경고 비활성화:
```makefile
CFLAGS += -Wno-array-bounds -Wno-infinite-recursion
```

---

## 5. 핵심 알고리즘

### 5.1 쓰기 흐름 (writei 호출 시)
```
1. 데이터 블록 기록
   └── lfs_alloc()으로 로그 tail에서 블록 할당
   └── bwrite()로 데이터 기록
   └── inode의 addrs[] 갱신

2. inode 기록
   └── lfs_alloc()으로 새 블록 할당
   └── dinode를 새 위치에 기록
   └── imap[inum] = 새 블록 주소

3. imap 기록
   └── imap 배열을 로그 tail에 기록
   └── checkpoint의 imap_addrs[] 갱신

4. checkpoint 기록
   └── log_tail, imap 위치 업데이트
   └── 고정 위치 (블록 2)에 기록
```

### 5.2 읽기 흐름 (ilock → readi)
```
1. imap에서 inode 위치 조회
   └── block = lfs.imap[inum]

2. 해당 위치에서 inode 읽기
   └── bp = bread(dev, block)
   └── inode 필드 복사

3. inode의 addrs[]로 데이터 블록 읽기
   └── 기존 xv6 readi와 동일
```

### 5.3 부팅 시 복구 (iinit)
```
1. Superblock 읽기
   └── LFS 매직 넘버 확인

2. Checkpoint 읽기 (블록 2)
   └── log_tail 복원
   └── imap 블록 위치 획득

3. Imap 로드
   └── checkpoint의 imap_addrs[]에서 imap 블록들 읽기
   └── 메모리 imap 배열 복원
```

---

## 6. 동기화 (Lock 처리)

### 6.1 문제점
xv6의 `bread()`는 sleep할 수 있으므로, spinlock을 잡은 상태에서 호출하면 안 됨.

### 6.2 해결 방법
Lock 범위를 최소화하고, I/O는 lock 밖에서 수행:

```c
// 예: iupdate()
acquire(&lfs.lock);
block = lfs.log_tail++;      // 빠른 연산만 lock 안에서
release(&lfs.lock);

bp = bread(ip->dev, block);  // I/O는 lock 밖에서
bwrite(bp);
brelse(bp);

acquire(&lfs.lock);
lfs.imap[ip->inum] = block;  // imap 갱신도 lock 안에서
release(&lfs.lock);
```

---

## 7. 테스트 결과

### 7.1 기본 테스트
```
$ ls                    # 디렉토리 읽기 ✓
$ cat README            # 파일 읽기 ✓
$ echo hello > test.txt # 파일 생성 ✓
$ cat test.txt          # 새 파일 읽기 ✓
hello
```

### 7.2 usertests -q 결과
```
ALL TESTS PASSED
```

통과한 테스트 목록:
- `createdelete` - 파일 생성/삭제
- `linkunlink` - 하드 링크
- `concreate` - 동시 파일 생성
- `fourfiles` - 다중 파일
- `sharedfd` - 파일 디스크립터 공유
- `bigwrite` - 대용량 쓰기
- `bigfile` - 대용량 파일
- `subdir` - 서브디렉토리
- `linktest` - 링크 테스트
- `unlinkread` - 언링크 후 읽기
- `bigdir` - 대용량 디렉토리
- `fork`, `exec`, `exitwait` - 프로세스 관리
- 기타 모든 테스트

---

## 8. 제한사항

### 8.1 GC 미구현
- 삭제된 파일의 블록이 회수되지 않음
- 로그가 디스크 끝에 도달하면 "out of disk space" panic
- 해결: FSSIZE를 충분히 크게 설정 (현재 50000 블록 = 25MB)

### 8.2 Crash-Safety 미구현
- 단일 checkpoint 사용
- 쓰기 중 크래시 시 파일 시스템 손상 가능
- 해결: checkpoint 교대 사용 구현 필요

### 8.3 성능 오버헤드
- 매 쓰기마다 imap + checkpoint 기록
- 작은 쓰기에도 여러 블록 사용
- **해결: 아이노드 버퍼링 최적화 적용 (아래 참조)**

---

## 9. 아이노드 버퍼링 최적화 (IPB = 8)

### 9.1 문제점
기존 구현에서는 inode 하나당 한 블록(512B)을 사용하여 공간 낭비가 심했음.
- dinode 크기: 64 바이트
- 블록 크기: 512 바이트
- 낭비: 512 - 64 = 448 바이트/inode

### 9.2 해결책: 8개 inode를 하나의 블록에 저장

#### imap 인코딩 (fs.h)
```c
// imap[inum] = (block_addr << 3) | slot_index
// slot_index: 0-7 (3 bits)
#define IMAP_SLOT_BITS 3
#define IMAP_SLOT_MASK ((1 << IMAP_SLOT_BITS) - 1)  // 0x7
#define IMAP_ENCODE(block, slot) (((block) << IMAP_SLOT_BITS) | ((slot) & IMAP_SLOT_MASK))
#define IMAP_BLOCK(entry) ((entry) >> IMAP_SLOT_BITS)
#define IMAP_SLOT(entry) ((entry) & IMAP_SLOT_MASK)
```

### 9.3 Imap Entry 하나로 Inode 블럭 자체를 가리킨다면?
- inode 8, inode 100 

#### Dirty Inode 버퍼 (fs.c)
```c
struct {
  struct spinlock lock;
  struct dinode inodes[IPB];   // 최대 8개 inode 버퍼
  uint inums[IPB];             // 해당 inode 번호
  int count;                   // 버퍼된 inode 수
} dirty_inodes;
```

### 9.3 동작 방식 (Sprite LFS 방식)

#### 쓰기 흐름
```
파일 A 쓰기:
  [데이터 A 기록] → inode A는 dirty 버퍼에 추가만 (sync 없음)

파일 B 쓰기:
  [데이터 B 기록] → inode B는 dirty 버퍼에 추가만 (sync 없음)

Sync 시점 (버퍼가 가득 찼을 때 - 8개):
  [dirty inodes (A+B+...)를 한 블록에 기록]
  [imap 기록]
  [checkpoint 기록]
```

로그 결과:
```
[데이터 A] [데이터 B] ... [inode 블록 (A+B+...)] [imap] [checkpoint]
```

#### iupdate() 동작
```c
void iupdate(struct inode *ip) {
  // 1. inode를 dirty 버퍼에 추가
  //    └── 이미 버퍼에 있으면 갱신
  //    └── 버퍼가 가득 차면 (8개) lfs_sync() 호출

  // 2. sync 없음 (Sprite LFS 방식)
  //    checkpoint는 버퍼가 가득 찼을 때만 기록
}
```

#### lfs_sync() 동작
```c
void lfs_sync(void) {
  lfs_flush_inodes();     // dirty inodes → 블록에 기록
  lfs_write_imap();       // imap 기록
  lfs_write_checkpoint(); // checkpoint 기록
}
```

#### 읽기 흐름 (ilock)
```
1. dirty 버퍼 확인
   └── 버퍼에 있으면 바로 반환

2. imap에서 위치 조회
   └── block = IMAP_BLOCK(imap[inum])
   └── slot = IMAP_SLOT(imap[inum])

3. 디스크에서 읽기
   └── bp = bread(dev, block)
   └── dip = (struct dinode*)bp->data + slot
```

### 9.4 효과
- **공간 효율**: 1개 inode당 64B (이전: 512B) - **8배 향상**
- **I/O 효율**: 데이터 먼저, inode는 나중에 모아서 기록
- mkfs 결과: log_tail이 ~585 블록

---

## 10. Sync 메커니즘

### 10.1 개요
Sprite LFS 방식에서는 dirty inode 버퍼가 가득 찼을 때만 sync가 발생한다.
그러나 버퍼가 가득 차지 않은 상태에서 시스템이 종료되면 데이터가 손실될 수 있다.
이를 방지하기 위해 두 가지 sync 메커니즘을 추가로 구현했다.

### 10.2 Sync 트리거 조건
| 조건 | 설명 | 파일 |
|------|------|------|
| **버퍼 Full** | dirty_inodes 버퍼가 8개 가득 찼을 때 | fs.c |
| **주기적 Sync** | 약 1초마다 (100 ticks) | trap.c |
| **Shutdown Sync** | panic() 호출 시 시스템 중단 전 | console.c |

### 10.3 주기적 Sync (trap.c)
타이머 인터럽트를 이용하여 약 1초마다 lfs_sync() 호출:

```c
// trap.c
#define LFS_SYNC_INTERVAL 100  // Sync every 100 ticks (~1 seconds)
static uint last_sync_tick = 0;

void trap(struct trapframe *tf) {
  // ... 인터럽트 처리 ...

  // 사용자 모드로 복귀 전 sync (sleep 가능한 컨텍스트)
  if(myproc() && cpuid() == 0 && ticks - last_sync_tick >= LFS_SYNC_INTERVAL){
    last_sync_tick = ticks;
    lfs_sync();
  }
}
```

**주의사항**:
- 타이머 인터럽트 핸들러 내부에서 직접 호출하면 안 됨 (bread()가 sleep 가능)
- 인터럽트 처리 후 사용자 모드로 복귀 전에 호출해야 함
- CPU 0에서만 실행하여 중복 sync 방지

### 10.4 Shutdown Sync (console.c)
panic() 발생 시 시스템 중단 전 데이터 보존:

```c
// console.c
void panic(char *s) {
  static int syncing = 0;  // Recursive sync 방지

  cli();
  cons.locking = 0;

  // LFS: 시스템 중단 전 sync
  if(!syncing && !panicked){
    syncing = 1;
    lfs_sync();
  }

  // ... panic 메시지 출력 및 halt ...
}
```

**주의사항**:
- syncing 플래그로 재귀 호출 방지 (lfs_sync() 내부에서 panic 발생 시)
- panicked 상태에서는 sync 시도 안 함

### 10.5 lfs_sync() 함수 (fs.c)
```c
void lfs_sync(void) {
  lfs_flush_inodes();     // dirty_inodes 버퍼를 디스크에 기록
  lfs_write_imap();       // imap을 로그에 기록
  lfs_write_checkpoint(); // checkpoint를 고정 위치에 기록
}
```

### 10.6 Sync 흐름도
```
일반 쓰기:
  writei() → iupdate() → dirty_inodes 버퍼에 추가
                              ↓
                    버퍼 Full (8개)?
                        ↓ Yes
                    lfs_sync()

주기적 Sync (1초):
  타이머 인터럽트 → trap() → lfs_sync()

Shutdown Sync:
  panic() → lfs_sync() → halt
```

### 10.7 문제 상황
1. echo hello > test1.txt
   - inum 18 할당 → dirty_inodes 버퍼에 추가
   - 디렉토리 엔트리 "test1.txt" -> 18 추가 (디스크에 기록됨!)
   - 하지만 imap/checkpoint는 아직 sync 안 됨

2. QEMU 종료
   - imap[18] = 디스크에 저장 안 됨

3. 재부팅
   - 디렉토리에는 test1.txt (inum 18) 존재
   - 하지만 imap[18] = 0
   - panic!


---

## 11. 파일 변경 요약

| 파일 | 추가 | 수정 | 삭제 |
|------|------|------|------|
| fs.h | +50 | +15 | -10 |
| param.h | +5 | - | - |
| fs.c | +300 | +150 | -50 |
| log.c | +20 | - | -180 |
| mkfs.c | +200 | +70 | -80 |
| trap.c | +10 | - | - |
| console.c | +8 | - | - |
| defs.h | +1 | - | - |
| Makefile | +2 | - | - |
| **총계** | **~600** | **~235** | **~320** |

---


## 12. 참고 자료

- Sprite LFS 논문: "The Design and Implementation of a Log-Structured File System" (Rosenblum & Ousterhout, 1992)
- xv6 소스코드: https://github.com/mit-pdos/xv6-public
