# custom-os — OS 개발 실습 프로젝트

직접 OS를 만들어보며 부트로더 → 보호모드 → 커널 → 드라이버 순서로 학습하는 실습 저장소.

## 환경

- **호스트**: Windows + WSL2
- **배포판**: Ubuntu 24.04.4 LTS (Noble) / 커널 `6.6.87.2-microsoft-standard-WSL2`
- **사용자 셸**: bash
- **작업 위치**: `~/workroot/src/custom-os/` (WSL 홈)
  - `/mnt/...` (Windows 드라이브 마운트) 경로는 컴파일이 느리고 권한·대소문자 이슈가 있어 피함.
  - 글/문서 작업은 `/mnt/f/workroot/articles/custom-os` 쪽에 별도로 존재할 수 있음.

## 툴체인

| 도구 | 상태 | 용도 |
|------|------|------|
| `gcc` 13.3 / `make` 4.3 | 기본 설치됨 | 호스트 컴파일러, 빌드 |
| `nasm` | **apt 설치 필요** | 어셈블리 (부트섹터 등) |
| `qemu-system-x86` | **apt 설치 필요** | 가상머신 (재부팅 없이 테스트) |
| `xorriso`, `grub-pc-bin`, `grub-common`, `mtools` | **apt 설치 필요** | ISO 이미지 / GRUB |
| `mtools`, `dosfstools` | **apt 설치 필요** | FAT 이미지 생성·복사 (`mkfs.fat`, `mcopy`) |
| `gcc-multilib`, `g++-multilib` | **apt 설치 필요** | 32비트 크로스 빌드 |
| `gdb` | **apt 설치 필요** | QEMU 원격 디버깅 (`-s -S`) |

일괄 설치:
```bash
sudo apt update
sudo apt install -y nasm qemu-system-x86 qemu-utils \
    xorriso grub-pc-bin grub-common mtools dosfstools gdb \
    gcc-multilib g++-multilib
```

GUI: WSL2 + WSLg(Windows 11)면 QEMU 창이 자동으로 뜸. 안 뜨면 `-nographic` 또는 `-display none -serial mon:stdio`로 콘솔만 사용.

## 실습 진행 방식

- 각 실습은 **번호 prefix 디렉토리**로 분리 (`01-hello-boot`, `02-protected-mode`, ...).
- 다음 단계로 갈 때는 **이전 단계 폴더를 복사한 뒤 확장**. 진행 흔적이 남아 비교가 쉬움.
- 각 디렉토리에 자체 `Makefile`을 두어 `make` / `make run` / `make clean`이 동작하게 함.
- 단계별 의도와 다음 할 일은 그 디렉토리의 `CLAUDE.md`에 둠.
- 각 단계의 `CLAUDE.md`에는 **이전 단계 대비 달라진 파일 목록**을 표로 정리한다 (신규/수정/삭제 구분, 한 줄 설명 포함).

### 예정 로드맵

| 번호 | 디렉토리 | 내용 |
|------|----------|------|
| 01 | `01-hello-boot` | 16비트 부트섹터에서 BIOS로 글자 출력 |
| 02 | `02-protected-mode` | 32비트 보호모드 진입, GDT |
| 03 | `03-a20` | A20 게이트 활성화 + 검증(1MB wraparound 테스트) |
| 04 | `04-disk-load` | `int 0x13` 디스크 로드(재시도/에러체크) → 적재한 추가 섹터로 점프 |
| 05 | `05-fat-load` | 부트로더가 FAT16에서 `KERNEL.BIN`을 찾아 적재 (BPB/루트 디렉토리/클러스터 체인) |
| 06 | `06-bios-lba` | BIOS `int 0x13` 확장 `AH=42`로 CHS 없이 LBA 읽기 |
| 07 | `07-c-kernel` | payload를 C 커널로 교체 — `linker.ld` 레이아웃 + asm 진입 stub→C 점프 |
| 08 | `08-grub-multiboot` | 같은 C 커널을 GRUB(Multiboot)로 부팅 (ISO) |
| 09 | `09-vga-print` | VGA 텍스트 모드 / `printf` 구현 |
| 10 | `10-interrupts` | PIC 리맵 + IDT + 인터럽트 핸들러 |
| 11 | `11-keyboard` | 키보드 드라이버 |
| 12 | `12-paging` | E820 메모리 맵 + 페이징, 가상 메모리 |
| 13 | `13-phys-mem` | E820 기반 물리 페이지 프레임 할당기 |
| 14 | `14-kernel-heap` | `kmalloc`/`kfree`와 커널 가상 메모리 확장 |
| 15 | `15-pit-timer` | PIT 타이머 IRQ0, tick 카운터, 간단한 sleep |
| 16 | `16-kernel-monitor` | 키보드 입력 기반 커널 모니터와 간단한 명령 파서 |
| 17 | `17-kernel-threads` | 커널 태스크 구조체, 문맥 전환, 첫 커널 쓰레드 실행 |
| 18 | `18-scheduler` | 라운드 로빈 스케줄러와 sleep/wakeup, 선점 기반 태스크 전환 |
| 19 | `19-user-mode` | GDT/TSS 정리, ring 3 진입, 첫 사용자 코드 실행 |
| 20 | `20-syscall` | `int 0x80` 기반 시스템 콜 진입 |
| 21 | `21-initramfs` | GRUB 모듈로 initramfs 적재, 메모리 파일 접근 |
| 22 | `22-elf-loader` | ELF 사용자 프로그램 적재와 첫 `init` 이미지 준비 |
| 23 | `23-expand-paging` | e820 전체 RAM identity map, 4MB → 동적 다중 page_table |
| 24 | `24-proc-addr-space` | per-process 페이지 디렉토리 클론, ELF 격리 적재 |
| 25 | `25-proc-spawn-exit` | `process_t`, proc_table, `proc_spawn`, `proc_exit` |
| 26 | `26-proc-wait` | `proc_wait`, 부모-자식 wait 흐름 |
| 27 | `27-proc-exec` | `SYS_EXEC` — 현재 프로세스 이미지를 새 ELF로 교체 |
| 28 | `28-proc-fork` | `SYS_FORK` — eager copy로 자식 생성, fork+exec 패턴 완성 |
| 29 | `29-vfs` | 파일 디스크립터, 경로 해석, 단순 VFS 계층 |
| 30 | `30-user-shell` | 사용자 모드 셸과 기본 명령 실행 |
| 31 | `31-pci` | PCI 버스 탐색과 장치 열거 |
| 32 | `32-net-driver` | NIC 드라이버와 Ethernet 프레임 송수신 |
| 33 | `33-net-ipv4` | ARP, IPv4, ICMP ping |
| 34 | `34-net-udp` | UDP 송수신과 간단한 소켓 API |
| 35 | `35-net-tcp` | 최소 TCP 연결과 사용자 공간 네트워크 프로그램 |

12 이후는 메모리 관리 → 타이머/커널 모니터 → 커널 쓰레드/스케줄링 → 사용자 모드/시스템 콜 → 사용자 프로그램 적재/프로세스 → 파일 시스템/셸 → PCI/네트워크 순서로 기반을 쌓는다.

순서·이름은 진행 중 자유롭게 조정 가능.

## 협업 규칙

- 사용자와의 대화 언어: **한국어**.
- 새 실습 단계를 만들 때는 폴더 생성 → `Makefile` → 소스 → `make run`까지 검증 후 마무리.
- **코드는 항상 이전 단계에서 "이어지는" 형태로 작성한다.** 새 단계를 백지에서 다시 쓰지 말 것:
  - 이전 단계의 코드 구조·레이블/식별자 이름·출력 메시지를 출발점으로 그대로 가져와 유지하고, 새 내용은 그 위에 *확장*으로 얹는다.
  - 출력 메시지도 학습 흐름이 드러나게 이어간다(예: 01의 `Hello world`를 02에서 `Hello world -- protected mode`로 발전시키는 식).
  - **연속성은 코드 자체(구조·이름·메시지)로 드러낸다. "이전 단계에서 이어짐", "01과 동일", "02에서 추가" 같은 연속성·유사성을 설명하는 주석은 달지 않는다.**
- **코드에는 주석을 하나도 달지 않는다.** 연속성 설명도, "왜 이 매직넘버/주소인지" 같은 비자명한 기술적 이유도 코드에 적지 말 것. 그런 근거·의도는 모두 그 단계의 `CLAUDE.md` 문서에 적는다.
