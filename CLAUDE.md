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
| `gcc-multilib`, `g++-multilib` | **apt 설치 필요** | 32비트 크로스 빌드 |
| `gdb` | **apt 설치 필요** | QEMU 원격 디버깅 (`-s -S`) |

일괄 설치:
```bash
sudo apt update
sudo apt install -y nasm qemu-system-x86 qemu-utils \
    xorriso grub-pc-bin grub-common mtools gdb \
    gcc-multilib g++-multilib
```

GUI: WSL2 + WSLg(Windows 11)면 QEMU 창이 자동으로 뜸. 안 뜨면 `-nographic` 또는 `-display none -serial mon:stdio`로 콘솔만 사용.

## 실습 진행 방식

- 각 실습은 **번호 prefix 디렉토리**로 분리 (`01-hello-boot`, `02-protected-mode`, ...).
- 다음 단계로 갈 때는 **이전 단계 폴더를 복사한 뒤 확장**. 진행 흔적이 남아 비교가 쉬움.
- 각 디렉토리에 자체 `Makefile`을 두어 `make` / `make run` / `make clean`이 동작하게 함.
- 단계별 의도와 다음 할 일은 그 디렉토리의 `CLAUDE.md`에 둠.

### 예정 로드맵

| 번호 | 디렉토리 | 내용 |
|------|----------|------|
| 01 | `01-hello-boot` | 16비트 부트섹터에서 BIOS로 글자 출력 |
| 02 | `02-protected-mode` | 32비트 보호모드 진입, GDT |
| 03 | `03-c-kernel` | Multiboot + C 커널 링크 |
| 04 | `04-vga-print` | VGA 텍스트 모드 / `printf` 구현 |
| 05 | `05-interrupts` | IDT, 인터럽트 핸들러 |
| 06 | `06-keyboard` | 키보드 드라이버 |
| 07 | `07-paging` | 페이징, 가상 메모리 |

순서·이름은 진행 중 자유롭게 조정 가능.

## 협업 규칙

- 사용자와의 대화 언어: **한국어**.
- 새 실습 단계를 만들 때는 폴더 생성 → `Makefile` → 소스 → `make run`까지 검증 후 마무리.
- 코드에 불필요한 주석/설명을 넣지 말 것. OS dev는 "왜 이 매직넘버/주소인지"가 비자명한 경우가 많으니 그런 곳에만 짧게 주석.
