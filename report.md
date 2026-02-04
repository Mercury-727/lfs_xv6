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


## 12. Garbage Collection (세그먼트 클리닝) 구현

### 12.1 개요

LFS는 모든 쓰기를 로그의 끝(tail)에 순차적으로 기록하므로, 파일이 삭제되거나 덮어써져도 이전 데이터가 디스크에 남아있다. 이러한 "죽은 블록(dead blocks)"을 회수하지 않으면 디스크가 가득 차게 된다. Garbage Collection(GC)은 이러한 죽은 블록을 포함하는 세그먼트를 선별하여, 그 안의 살아있는 블록만 새 위치로 이동(relocate)한 후, 전체 세그먼트를 재사용 가능하도록 반환하는 과정이다.

본 구현은 Sprite LFS 논문의 **cost-benefit 정책**과 **세그먼트 요약 블록(SSB) 기반 라이브 감지** 방식을 따른다.

### 12.2 GC를 위해 추가된 메타데이터

#### 12.2.1 Imap 인코딩 변경 (fs.h)

GC가 블록의 "활성(live)" 여부를 판단하려면 **버전(version)** 정보가 필요하다. 기존의 `(block << 3) | slot` 인코딩에서 버전 필드를 추가하여 3-필드 인코딩으로 변경했다.

```c
// 변경 전: imap[inum] = (block_addr << 3) | slot_index  (3비트 슬롯)
// 변경 후: imap[inum] = (block_addr << 12) | (version << 4) | slot_index

#define IMAP_SLOT_BITS    4    // 슬롯 인덱스: 0-15 (4비트)
#define IMAP_VERSION_BITS 8    // 버전: 0-255 (8비트)
#define IMAP_ENCODE(block, version, slot) \
  (((block) << 12) | (((version) & 0xFF) << 4) | ((slot) & 0xF))
#define IMAP_BLOCK(entry)   ((entry) >> 12)
#define IMAP_VERSION(entry) (((entry) >> 4) & 0xFF)
#define IMAP_SLOT(entry)    ((entry) & 0xF)
```

**용도**: SSB의 `version` 필드와 imap의 `version`을 비교하여 해당 블록이 현재 유효한 버전에 속하는지 판단한다. 버전이 불일치하면 해당 블록은 죽은 것으로 간주하고 이동하지 않는다.

#### 12.2.2 Segment Summary Block — SSB (fs.h)

세그먼트에 기록된 각 블록이 어떤 inode의 어떤 오프셋에 해당하는지를 기록하는 메타데이터 블록이다.

```c
// SSB 엔트리: 각 데이터 블록에 대한 소유 정보
struct ssb_entry {
  uint inum;      // 이 블록을 소유한 inode 번호
  uint offset;    // 파일 내 블록 오프셋 (SSB_INODE_BLOCK_MARKER이면 inode 블록)
  uint version;   // 기록 시점의 inode 버전
};

// SSB 블록 헤더 (on-disk)
#define SSB_MAGIC  0x53534221  // "SSB!" — SSB 블록 식별 매직 넘버
#define SSB_ENTRIES_PER_BLOCK  ((BSIZE - 3*sizeof(uint)) / sizeof(struct ssb_entry))  // 84개

struct ssb {
  uint magic;      // SSB_MAGIC
  uint nblocks;    // 이 SSB가 설명하는 데이터 블록 수
  uint checksum;   // 엔트리들의 XOR 기반 체크섬 (무결성 검증)
  struct ssb_entry entries[SSB_ENTRIES_PER_BLOCK];
};
```

**SSB 기록 시점**: `lfs_sync()` 호출 시 `lfs_flush_ssb()`가 현재 SSB 버퍼를 디스크에 기록한다.

**특수 오프셋 상수**:
```c
#define SSB_INODE_BLOCK_MARKER  0xFFFFFFFF  // offset 필드에 이 값이면 inode 블록
```

#### 12.2.3 Segment Usage Table — SUT (fs.h, fs.c)

각 세그먼트의 활성 바이트 수와 마지막 수정 시각을 추적하는 테이블이다.

```c
// On-disk 엔트리
struct sut_entry {
  uint live_bytes;  // 세그먼트 내 활성 데이터 크기 (바이트)
  uint age;         // 마지막 수정 시각 (ticks)
};

// 관련 상수
#define NSUT_BLOCKS     64    // 최대 SUT 디스크 블록 수
#define LFS_NSEGS_MAX   4000  // 최대 세그먼트 수
#define SUT_FREE_MARKER 0xFFFFFFFF  // GC가 회수한 세그먼트 표시
```

**런타임 상태** (`lfs` 구조체 내):
```c
struct sut_entry sut[LFS_NSEGS_MAX];  // 메모리 내 SUT
```

**SUT 갱신**: `lfs_update_usage(block_addr, delta)` 함수가 블록 할당 시 `+BSIZE`, 블록 해제/덮어쓰기 시 `-BSIZE`를 적용한다.

**SUT 디스크 기록**: `lfs_write_sut()` 함수가 checkpoint 전에 SUT 블록들을 로그에 기록한다. 변경되지 않은 블록은 재기록하지 않는 **부분 갱신(partial update)** 최적화를 적용했다.

#### 12.2.4 Checkpoint 확장 (fs.h)

기존 checkpoint에 SUT 블록 위치 필드를 추가했다.

```c
struct checkpoint {
  uint timestamp;
  uint log_tail;
  uint cur_seg;
  uint seg_offset;
  uint imap_addrs[NIMAP_BLOCKS];
  uint imap_nblocks;
  uint sut_addrs[NSUT_BLOCKS];    // ← GC를 위해 추가: SUT 블록들의 디스크 주소
  uint sut_nblocks;                // ← GC를 위해 추가: SUT 블록 수
  uint valid;
};
```

#### 12.2.5 LFS 런타임 상태 확장 (fs.c)

`lfs` 구조체에 GC 관련 필드를 추가했다.

```c
struct {
  struct spinlock lock;
  uint imap[LFS_NINODES];
  struct checkpoint cp;
  uint log_tail;
  uint cur_seg_end;                      // ← 현재 할당 가능 영역의 끝
  int dev;
  int syncing;

  // GC / SUT 상태
  struct sut_entry sut[LFS_NSEGS_MAX];   // ← 세그먼트 사용량 테이블
  struct ssb_entry ssb_buf[LFS_SEGSIZE]; // ← 현재 세그먼트의 SSB 버퍼
  uint ssb_count;                        // ← SSB 버퍼 내 엔트리 수

  // 프리 세그먼트 순환 버퍼 (GC가 회수한 세그먼트)
  uint free_segs[LFS_NSEGS_MAX];         // ← 프리 세그먼트 인덱스 배열
  int free_head;                         // ← 순환 버퍼 head
  int free_tail;                         // ← 순환 버퍼 tail
  int free_count;                        // ← 프리 세그먼트 수

  int gc_running;                        // ← GC 재진입 방지 플래그
  int gc_failed;                         // ← GC 실패 플래그 (무한 트리거 방지)

  // 펜딩 프리 세그먼트 (checkpoint sync 대기)
  uint pending_free_segs[GC_TARGET_SEGS]; // ← 체크포인트 전 대기 목록
  int pending_free_count;
} lfs;
```

#### 12.2.6 Dirty Inode 버퍼 확장 (fs.c)

GC가 inode를 재배치할 때 올바른 버전으로 imap에 기록하기 위해 `versions[]` 필드를 추가했다.

```c
struct {
  struct spinlock lock;
  struct dinode inodes[IPB];
  uint inums[IPB];
  uint versions[IPB];          // ← GC를 위해 추가: 각 inode의 버전
  int count;
  // Flushing buffer (sync 중 사용)
  struct dinode flushing_inodes[IPB];
  uint flushing_inums[IPB];
  uint flushing_versions[IPB]; // ← GC를 위해 추가
  int flushing_count;
} dirty_inodes;
```

#### 12.2.7 GC 관련 상수 (param.h)

```c
#define GC_THRESHOLD       50   // GC 트리거 임계값 (디스크 사용률 %)
#define GC_TARGET_SEGS      4   // GC 1회 실행 시 클리닝할 세그먼트 수
#define GC_UTIL_THRESHOLD  80   // 클리닝 대상 최대 활용률 (%)
```

#### 12.2.8 Victim 선택 구조체 (fs.c)

```c
struct gc_victim {
  uint seg_idx;       // 세그먼트 인덱스
  uint score;         // cost-benefit 점수 (높을수록 우선)
  uint util_percent;  // 세그먼트 활용률 (%)
};
```

---

### 12.3 GC 알고리즘

#### 12.3.1 GC 트리거 조건 (`lfs_alloc`)

블록 할당 시 다음 두 가지 조건 중 하나를 만족하면 GC를 실행한다:

1. **프리 세그먼트 부족** (`free_count < GC_TARGET_SEGS`)이면서 `cur_seg_end < sb.size` (이미 프리 세그먼트를 사용 중 = 초기 연속 영역 소진)
2. **디스크 사용률** ≥ `GC_THRESHOLD`(50%)

단, `gc_running`(재진입 방지), `syncing`(sync 중), `gc_failed`(이전 GC 실패) 플래그가 모두 0일 때만 트리거된다.

#### 12.3.2 Victim 선택 (`gc_select_victims`)

**Cost-Benefit 정책** (Sprite LFS 논문):

```
score = (100 - u) × age / (100 + u) × 1000
```

- `u`: 세그먼트 활용률 (%) — `live_bytes / (segsize × BSIZE) × 100`
- `age`: 세그먼트 나이 — `ticks - sut[seg].age`
- 활용률 100%인 세그먼트는 score = 0 (선택 불가)
- 활용률 > `GC_UTIL_THRESHOLD`(80%)인 세그먼트는 스킵
- `SUT_FREE_MARKER`로 표시된 세그먼트는 스킵 (이미 회수됨)
- 현재 쓰기 중인 세그먼트는 스킵

점수가 높은 순으로 최대 `GC_TARGET_SEGS`(4)개의 세그먼트를 선택한다.

#### 12.3.3 세그먼트 클리닝 (`gc_clean_segment`)

```
1. SSB 탐색 (gc_find_ssbs)
   └── 세그먼트 내 모든 블록을 스캔하여 SSB_MAGIC을 찾음
   └── 체크섬 검증으로 무결성 확인

2. SSB 기반 라이브 블록 재배치
   └── 각 SSB 엔트리에 대해:
       ├── 버전 확인: imap 버전 ≠ SSB 버전이면 skip (dead)
       ├── inode 블록 (offset = SSB_INODE_BLOCK_MARKER):
       │   └── gc_relocate_inode_block(): 블록 복사 + imap 갱신
       └── 데이터 블록:
           └── inode에서 실제 블록 주소 확인
           └── gc_relocate_block(): 블록 복사 + inode addrs[] 갱신

3. 안전 스캔 (Safety Scan) — SSB 누락 시 fallback
   ├── 조건: SSB가 아예 없는 세그먼트 (ssb_count == 0)에서만 실행
   ├── 3a. imap 전체 스캔: 이 세그먼트에 있는 inode 블록 찾기
   │   └── gc_relocate_inode_block()으로 재배치
   └── 3b. 모든 inode의 데이터 블록 스캔:
       ├── direct 블록 (addrs[0..11])
       ├── indirect 블록 자체 (addrs[NDIRECT])
       └── indirect 블록 내 데이터 블록 (addrs[NDIRECT][0..255])

   **참고**: Bug 7 수정 후 mkfs도 올바른 SSB를 생성하므로,
   안전 스캔은 거의 실행되지 않음 (SSB 손상 시에만 필요)

4. 세그먼트 해제 (gc_free_segment)
   └── pending_free_segs[]에 추가
   └── sut[seg].live_bytes = SUT_FREE_MARKER
```

#### 12.3.4 메인 GC 흐름 (`lfs_gc`)

```
1. gc_running 플래그 설정 (재진입 방지)
2. gc_select_victims()으로 victim 선택
3. 각 victim에 대해 gc_clean_segment() 호출
4. lfs_sync()로 변경사항 디스크에 커밋
5. pending_free_segs[] → free_segs[] 순환 버퍼로 이동
   (checkpoint가 기록된 후에만 실제 재사용 가능)
6. gc_running = 0, gc_failed = 0 해제
```

#### 12.3.5 Sync 흐름 확장

GC 추가 후 `lfs_sync()`의 흐름:

```
lfs_sync()
  ├── lfs_flush_inodes()   — dirty inode 버퍼 → 디스크
  ├── lfs_flush_ssb()      — SSB 버퍼 → 디스크 (★ GC 추가)
  ├── lfs_write_sut()      — SUT → 디스크 (★ GC 추가)
  ├── lfs_write_imap()     — imap → 디스크
  └── lfs_write_checkpoint() — checkpoint → 고정 위치
```

---

### 12.4 GC 관련 함수 목록

| 함수 | 역할 |
|------|------|
| `lfs_gc()` | GC 메인 함수: victim 선택 → 클리닝 → sync |
| `gc_select_victims()` | cost-benefit 정책으로 victim 세그먼트 선택 |
| `gc_cost_benefit()` | 세그먼트의 클리닝 점수 계산 |
| `gc_clean_segment()` | 단일 세그먼트 클리닝 (SSB 파싱 + 블록 재배치 + 안전 스캔) |
| `gc_find_ssbs()` | 세그먼트 내 SSB 블록 탐색 (매직 넘버 + 체크섬) |
| `gc_relocate_block()` | 데이터 블록 재배치 (direct/indirect + COW) |
| `gc_relocate_inode_block()` | inode 블록 재배치 (imap 갱신) |
| `gc_free_segment()` | 세그먼트를 pending 목록에 추가 |
| `gc_compute_checksum()` | SSB 엔트리 XOR 체크섬 계산 |
| `gc_verify_checksum()` | SSB 체크섬 검증 |
| `lfs_update_usage()` | SUT live_bytes 갱신 (+/- delta) |
| `lfs_add_ssb_entry()` | SSB 버퍼에 엔트리 추가 |
| `lfs_flush_ssb()` | SSB 버퍼를 디스크에 기록 |
| `lfs_write_sut()` | SUT를 디스크에 기록 (부분 갱신 최적화) |

---

## 13. GC 구현 과정의 애로사항 (디버깅 기록)

GC 구현 과정에서 발견하고 수정한 6개의 버그를 기술한다.

### 13.1 Bug 1: `cur_seg_end` 바운더리 오버플로

**증상**: GC가 프리 세그먼트를 할당한 후 `lfs_alloc()`이 프리 세그먼트 범위를 넘어서 계속 할당하여 다른 세그먼트의 데이터를 덮어씀.

**원인**: `lfs_alloc()`에 할당 가능 영역의 끝을 추적하는 변수가 없었다. `log_tail`만 증가시키면서, 프리 세그먼트의 끝을 넘어서도 계속 할당했다.

**수정**: `lfs` 구조체에 `cur_seg_end` 필드를 추가하여 현재 할당 가능한 영역의 끝을 추적한다. 초기값은 `sb.size`(디스크 끝)이고, 프리 세그먼트를 사용할 때 `cur_seg_end = log_tail + sb.segsize`로 설정한다. `log_tail >= cur_seg_end`이면 다음 프리 세그먼트로 전환한다.

### 13.2 Bug 2: GC 트리거 조건 오류

**증상**: 디스크가 가득 찼는데도 GC가 트리거되지 않아 `panic: out of disk space`.

**원인**: GC 트리거 조건이 `usage_percent >= GC_THRESHOLD`만 확인했으나, 이미 프리 세그먼트를 소비 중인 상태(= 초기 연속 영역이 소진됨)를 감지하지 못했다.

**수정**: 프리 세그먼트를 사용 중인 경우(`cur_seg_end < sb.size`) GC를 즉시 트리거하는 조건을 추가했다. 또한 GC가 victim을 찾지 못한 경우 `gc_failed` 플래그를 설정하여 무한 GC 루프를 방지했다.

### 13.3 Bug 3: `gc_select_victims` 스캔 범위 오류

**증상**: GC가 일부 세그먼트만 스캔하고 나머지를 놓침.

**원인**: victim 선택 시 `0..cur_seg`까지만 스캔했다. 그러나 프리 세그먼트를 사용하면 `cur_seg`는 낮은 인덱스일 수 있으므로, 높은 인덱스의 세그먼트가 누락되었다.

**수정**: 스캔 범위를 `0..sb.nsegs`로 변경하여 모든 세그먼트를 대상으로 스캔한다.

### 13.4 Bug 4: mkfs 블록에 SSB가 없는 문제

**증상**: GC가 mkfs가 생성한 초기 블록(부트 파일, 유저 프로그램 등)이 포함된 세그먼트를 클리닝할 때, 해당 블록들이 SSB에 기록되어 있지 않아 live block으로 인식하지 못하고 데이터 손실 발생. 이후 `ilock: no type` panic.

**원인**: mkfs는 SSB 없이 블록을 직접 기록한다. GC는 SSB만 참조하여 라이브 블록을 찾으므로, SSB가 없는 블록은 dead로 간주되어 회수되었다.

**수정**: `gc_clean_segment`에 **안전 스캔(safety scan)** 단계를 추가했다:
- **3a**: imap 전체를 스캔하여, 이 세그먼트에 남아있는 inode 블록을 찾아 재배치
- **3b**: 모든 inode의 `addrs[]`를 스캔하여, 이 세그먼트에 남아있는 데이터 블록과 indirect 블록을 찾아 재배치

이 안전 스캔은 SSB가 없는 블록뿐만 아니라, SSB 파싱 누락 등의 예외 상황도 처리한다.

### 13.5 Bug 5: Indirect 블록 재배치 시 데이터 손상

**증상**: `gctest` 실행 후 `usertests -q`의 `bigarg` 테스트에서 page fault (`trap 14`) 발생. EIP가 명령어 중간을 가리킴 — 코드 자체가 디스크에서 손상된 채 로드됨.

**원인**: `gc_clean_segment`의 안전 스캔 3b 단계에서, indirect 블록 자체가 세그먼트에 있을 때 `gc_relocate_block(offset=NDIRECT)` 를 호출했다. 그런데 `gc_relocate_block`은 `offset >= NDIRECT`를 "indirect 블록 내 데이터 블록(`indirect[offset - NDIRECT]`)"으로 해석하므로:

```
offset = NDIRECT = 12  →  bn = 12  →  bn >= NDIRECT이므로 indirect 경로 진입
  → indirect[12 - 12] = indirect[0]에 해당하는 데이터 블록으로 취급
  → old indirect를 읽고, COW 복사본 생성
  → new_indirect[0] = new_block (= indirect 블록 데이터의 복사본)
  → 결과: indirect[0]이 실제 첫 번째 indirect 데이터 블록 대신 indirect 블록 자체의 복사본을 가리킴
```

이로 인해 파일의 12번째 블록(첫 번째 indirect 엔트리)이 손상되어, `_usertests` 바이너리의 코드가 깨졌다.

**디버깅 과정**:
1. `objdump -d _usertests`로 디스어셈블하여 EIP 0x3382가 명령어 중간임을 확인
2. 가상 주소 0x3382를 파일 오프셋으로 변환 → 블록 12 = NDIRECT = 첫 indirect 엔트리
3. `gc_relocate_block`의 `offset >= NDIRECT` 분기 로직을 추적하여 원인 확인

**수정**: indirect 블록 자체의 재배치는 `gc_relocate_block()`을 사용하지 않고, **인라인 코드**로 직접 처리한다:
1. 구(old) indirect 블록을 읽음
2. log_tail에 새 블록 할당
3. 데이터를 그대로 복사 (엔트리 수정 없음)
4. SUT 갱신
5. inode의 `addrs[NDIRECT]`를 새 블록으로 갱신 (dirty buffer + icache)

### 13.6 Bug 6: GC 중 Sleeplock 교착 상태 (Deadlock)

**증상**: Fix 5 적용 후, `gctest → usertests -q` 실행 시 `sharedfd` 테스트에서 panic이 발생하거나, `many creates, followed by unlink` 테스트에서 무한 대기(hang).

**원인**: GC는 `lfs_alloc()` 내부에서 호출되고, `lfs_alloc()`은 `bmap()` → `writei()` 경로에서 호출된다. 이 시점에서 현재 프로세스가 이미 특정 inode의 sleeplock을 잡고 있다.

GC의 icache 갱신 코드가 `acquiresleep(&ip->lock)`을 호출하면, 동일한 inode에 대해 sleeplock을 이중으로 획득하게 되어 교착 상태가 발생한다 (xv6의 sleeplock은 재진입 불가).

```
writei(inode A) — inode A lock 보유
  → bmap() → lfs_alloc() → lfs_gc()
    → gc_relocate_block(inode A의 블록)
      → acquiresleep(&inode_A->lock)  ← 교착!
```

**수정**: GC의 icache 갱신 코드 3곳에서 `acquiresleep`/`releasesleep`을 제거하고, `icache.lock` (spinlock)만 잡은 상태에서 `addrs[]`를 직접 갱신한다:

```c
// 변경 전 (교착 발생):
if(cached_ip != 0){
  acquiresleep(&cached_ip->lock);
  cached_ip->addrs[bn] = new_block;
  releasesleep(&cached_ip->lock);
}

// 변경 후 (교착 없음):
acquire(&icache.lock);
for(struct inode *ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++){
  if(ip->ref > 0 && ip->dev == lfs.dev && ip->inum == target_inum){
    ip->addrs[...] = new_value;
    break;
  }
}
release(&icache.lock);
```

단일 `uint` 대입은 x86에서 원자적이고, `icache.lock`이 동시 접근을 방지하므로 안전하다.

### 13.7 Bug 7: SSB_TYPE_DATA = 0 정의 오류

**증상**: `gctest` 2회 실행 후 `usertests -q`의 `exec test`에서 `exec echo failed` 발생. 디버깅 결과 echo 파일의 ELF magic이 0x464c457f가 아닌 garbage 값(0x124 등)으로 읽힘.

**원인**: `fs.h`에서 SSB 블록 타입 상수가 다음과 같이 정의되어 있었다:

```c
#define SSB_TYPE_DATA     0   // ← 문제!
#define SSB_TYPE_INODE    1
#define SSB_TYPE_INDIRECT 2
```

`mkfs.c`의 `lfs_alloc_with_ssb()` 함수에서 SSB 엔트리 추가 조건이 `if(type != 0)`이었기 때문에, `SSB_TYPE_DATA`(= 0)로 호출된 데이터 블록에 대해서는 **SSB 엔트리가 전혀 생성되지 않았다**.

결과적으로 mkfs가 생성한 모든 파일(echo, ls, cat 등)의 **데이터 블록**에 대한 SSB 엔트리가 없었고, GC가 해당 세그먼트를 클리닝할 때 이 블록들을 live로 인식하지 못해 덮어쓰기가 발생했다.

**디버깅 과정**:
1. `exec.c`에 디버그 출력 추가: inode 정보와 ELF magic 값 출력
2. `mkfs.c`에 SSB flush 시 엔트리 내용 출력 추가
3. 출력 결과 분석:
   - segment 0의 SSB에 entry가 1개뿐 (데이터 블록 없음)
   - echo(inum=4)의 `offset=12`만 segment 1 SSB에 존재
   - `offset=0~11`에 대한 SSB 엔트리 없음
4. `SSB_TYPE_DATA = 0` 발견 → `if(type != 0)` 조건으로 인해 skip됨

**수정**: SSB 타입 상수를 모두 non-zero로 변경:

```c
// 변경 전:
#define SSB_TYPE_DATA     0
#define SSB_TYPE_INODE    1
#define SSB_TYPE_INDIRECT 2

// 변경 후:
#define SSB_TYPE_DATA     1
#define SSB_TYPE_INODE    2
#define SSB_TYPE_INDIRECT 3
```

### 13.8 Bug 8: 새 inode의 imap 미설정

**증상**: `gctest` 3회 실행 후 `ls` 명령에서 `ilock: inum 35 not in imap` panic 발생.

**원인**: `lfs_flush_inodes()`에서 imap 업데이트 시 다음 조건을 사용했다:

```c
if(lfs.imap[inum] != 0) {  // Check if inode is still valid (not freed)
  lfs.imap[inum] = IMAP_ENCODE(block, version, i);
}
```

이 조건의 의도는 "삭제된 inode는 imap에 기록하지 않음"이었으나, **새로 생성된 inode**도 처음에는 `imap[inum] = 0`이므로 이 조건을 통과하지 못했다.

결과:
1. `gctest`가 파일 생성 → inode 35 할당 → dirty_inodes 버퍼에 추가
2. `lfs_flush_inodes()` 호출 → `imap[35] == 0`이므로 skip
3. inode 35는 디스크에 기록되지만 imap[35]는 0으로 유지
4. 나중에 `ls`가 디렉토리 읽기 → inum 35 접근 시도 → imap[35] = 0 → panic

**수정**: imap 값 대신 **inode의 type 필드**를 체크하도록 변경:

```c
// 변경 전:
if(lfs.imap[inum] != 0) {
  lfs.imap[inum] = IMAP_ENCODE(block, version, i);
}

// 변경 후:
if(dirty_inodes.flushing_inodes[i].type != 0) {
  lfs.imap[inum] = IMAP_ENCODE(block, version, i);
}
```

- 새 inode: `type != 0` → imap 설정됨 ✓
- 삭제된 inode: `type == 0` → imap 설정 안 됨 ✓

### 13.9 Bug 9: SSB_TYPE_INDIRECT 블록 이중 복사

**증상**: `gctest` 4회 실행 후 GC 로그에 `INVALID block_addr=1145258561` (garbage 값) 경고 출력. 파일 시스템은 정상 동작하나 비효율적.

**원인**: `gc_relocate_block()`에서 SSB_TYPE_INDIRECT 엔트리 처리 시 잘못된 분기로 진입했다.

```c
bn = entry->offset;  // SSB_TYPE_INDIRECT의 경우 bn = NDIRECT = 12

if(bn < NDIRECT){
  // direct block 처리
  dirty_inodes.inodes[idx].addrs[bn] = new_block;
} else {
  // indirect 내 데이터 블록 처리 (COW 필요)
  // ← SSB_TYPE_INDIRECT도 여기로 진입!
}
```

`SSB_TYPE_INDIRECT`의 `offset`은 `NDIRECT`(= 12)이므로 `bn >= NDIRECT` 조건을 만족하여 **indirect 내 데이터 블록 처리 경로**로 진입했다.

이 경로는:
1. 새 데이터 블록 `new_block` 할당 (line 855)
2. 데이터 복사 (line 869)
3. **또 다른** indirect 블록 `new_ind` 할당 (line 978)
4. indirect 블록 전체 복사 (line 999)
5. `addrs[NDIRECT] = new_ind` 업데이트

결과적으로 indirect 블록이 **두 번** 복사되고, 첫 번째 복사본(`new_block`)은 orphaned 상태가 됨.

**수정**: `SSB_TYPE_INDIRECT`를 direct block과 같은 방식으로 처리:

```c
// 변경 전:
if(bn < NDIRECT){
  dirty_inodes.inodes[idx].addrs[bn] = new_block;
} else {
  // indirect 내 데이터 블록 COW 처리...
}

// 변경 후:
if(bn < NDIRECT || entry->type == SSB_TYPE_INDIRECT){
  if(entry->type == SSB_TYPE_INDIRECT){
    dirty_inodes.inodes[idx].addrs[NDIRECT] = new_block;
  } else {
    dirty_inodes.inodes[idx].addrs[bn] = new_block;
  }
} else {
  // indirect 내 DATA 블록 COW 처리...
}
```

동일한 수정을 `read_from_disk` 경로에도 적용.

---

## 14. 최종 테스트 결과

### 14.1 테스트 시나리오

GC가 여러 번 트리거되는 스트레스 환경에서 전체 테스트를 수행:

```
1. gctest × 4회  — 40개 파일(32KB) 생성 → 20개 삭제 → 20개 재생성 (GC 반복 트리거)
2. usertests -q  — 전체 사용자 테스트 (28개 테스트)
3. ls            — 파일 시스템 무결성 확인
4. echo hi       — mkfs 생성 파일의 무결성 확인
```

### 14.2 결과

```
$ gctest        # 1차
$ gctest        # 2차
$ gctest        # 3차
$ gctest        # 4차
$ usertests -q
...
ALL TESTS PASSED
$ ls
.              1 1 9760
..             1 1 9760
README         2 2 2286
cat            2 3 15300
echo           2 4 14204
...
$ echo hi
hi
```

모든 28개 usertests 통과:
- `createdelete`, `linkunlink`, `concreate`, `fourfiles`, `sharedfd`
- `bigarg` (×2회), `bigwrite`, `bss`, `sbrk`
- `validate`, `open`, `small file`, `big files`
- `many creates/unlink`, `openiput`, `exitiput`, `iput`
- `mem`, `pipe1`, `preempt`, `exitwait`, `rmdot`
- `fourteen`, `bigfile`, `subdir`, `linktest`, `unlinkread`
- `dir vs file`, `empty file name`, `fork`, `bigdir`, `uio`, `exec`

**특히 주목할 점**: GC가 segment 0 (mkfs 생성 파일 포함)을 여러 번 클리닝한 후에도 `echo`, `ls` 등의 시스템 유틸리티가 정상 동작함.

### 14.3 디스크 파라미터

| 파라미터 | 값 | 설명 |
|----------|-----|------|
| FSSIZE | 10000 | 디스크 크기 (블록) |
| BSIZE | 1024 | 블록 크기 (바이트) |
| LFS_SEGSIZE | 64 | 세그먼트 크기 (블록) |
| nsegs | 156 | 세그먼트 수 |
| segstart | 4 | 로그 시작 블록 |
| LFS_NINODES | 200 | 최대 inode 수 |

---

## 15. 제한사항 (갱신)

### 15.1 GC 구현 완료
- ~~GC 미구현~~ → Sprite LFS cost-benefit 정책 기반 GC 구현 완료
- 프리 세그먼트 순환 버퍼를 통한 세그먼트 재활용
- mkfs도 올바른 SSB 엔트리 생성 (Bug 7 수정 후)

### 15.2 Crash-Safety 미구현 (기존 유지)
- 단일 checkpoint 사용
- 쓰기 중 크래시 시 파일 시스템 손상 가능

### 15.3 GC 성능 특성
- SSB 기반 라이브 블록 감지로 효율적인 세그먼트 클리닝
- 안전 스캔은 SSB가 없는 세그먼트에서만 실행 (일반적으로 불필요)
- SUT 부분 갱신 최적화 적용됨

### 15.4 알려진 경고 (정상 동작)
- GC가 이전에 이미 relocate된 블록의 stale SSB 엔트리를 만나면 `INVALID block_addr` 경고 출력
- 이는 version mismatch로 인해 skip되므로 데이터 무결성에 영향 없음
- 경고는 정보 제공 목적이며, 향후 버전에서 제거 가능

---

## 16. 버그 수정 요약

총 9개의 버그를 발견하고 수정함:

| # | 버그 | 증상 | 원인 | 수정 |
|---|------|------|------|------|
| 1 | cur_seg_end 오버플로 | 세그먼트 경계 넘어 할당 | 할당 영역 끝 미추적 | `cur_seg_end` 필드 추가 |
| 2 | GC 트리거 오류 | 디스크 풀인데 GC 안 됨 | 프리 세그먼트 상태 미감지 | 트리거 조건 보강 |
| 3 | victim 스캔 범위 | 일부 세그먼트 누락 | `0..cur_seg`만 스캔 | `0..sb.nsegs` 스캔 |
| 4 | mkfs SSB 미생성 | mkfs 파일 손상 | SSB 없어 dead로 처리 | Safety Scan 추가 |
| 5 | indirect 재배치 손상 | 코드 손상, trap 14 | offset 해석 오류 | 인라인 처리로 변경 |
| 6 | sleeplock 교착 | hang 또는 panic | GC 중 inode lock 재획득 | spinlock만 사용 |
| 7 | **SSB_TYPE_DATA=0** | `exec echo failed` | DATA 블록 SSB 미생성 | 상수값 1로 변경 |
| 8 | **새 inode imap 미설정** | `inode not in imap` | imap[]=0 조건 오류 | type 체크로 변경 |
| 9 | **INDIRECT 이중 복사** | INVALID block_addr 경고 | 잘못된 분기 진입 | 분기 조건 수정 |

**핵심 교훈**:
- 상수 정의 시 0을 특별한 의미("없음")로 사용할 때 주의 필요
- 새로 생성되는 객체와 삭제된 객체의 초기값이 같을 때 구분 방법 고려 필요
- 블록 타입(DATA, INODE, INDIRECT)에 따른 처리 경로 명확히 분리 필요

---

## 17. 참고 자료

- Sprite LFS 논문: "The Design and Implementation of a Log-Structured File System" (Rosenblum & Ousterhout, 1992)
- xv6 소스코드: https://github.com/mit-pdos/xv6-public
