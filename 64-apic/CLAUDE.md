# 64 — apic

**목표**: `10-interrupts`가 초기화한 8259 PIC 기반 인터럽트 컨트롤러를 IOAPIC(MMIO)로 대체하고, Local APIC은 CPUID로 x2APIC 지원 여부를 게이팅한 뒤 **x2APIC(MSR 기반)을 기본**으로 삼는다(미지원 시 xAPIC MMIO로 폴백). `63-multiboot2`에서 확보만 해두고 미뤘던 ACPI RSDP를 이번 단계에서 실제로 RSDT/XSDT → MADT까지 파싱해 IOAPIC/Local APIC 베이스 주소와 레거시 IRQ 라우팅 정보를 뽑아낸다. IOAPIC(디바이스 IRQ 라우팅)과 Local APIC(코어별 인터럽트 수신·EOI)은 서로 다른 하드웨어 부품이라, xAPIC/x2APIC 구분은 Local APIC에만 해당하고 IOAPIC은 그 구분과 무관하게 항상 MMIO(`0xFEC00000` 근방)로 접근한다.

CPUID 명령어는 이 프로젝트에 지금까지 한 번도 등장한 적 없다(`37-long-mode`도 CPUID로 롱모드 지원을 확인하지 않고 바로 `EFER.LME`를 켰다) — `64-apic`가 CPUID를 처음 도입하는 지점이다. MSR(`rdmsr`/`wrmsr`)은 이미 `37`(EFER.LME), `43`(FS_BASE), `44`(SYSCALL MSR)에서 반복 사용된 개념이라 x2APIC의 MSR 인터페이스 자체는 새로울 게 없다.

## 1) `boot/acpi.c`/`.h`: RSDP → RSDT/XSDT → MADT

`63-multiboot2`가 남긴 `rsdp_tag->rsdp[]` 바이트를 `kernel_main`에서 `acpi_init()`에 그대로 넘긴다. RSDP의 `revision` 필드(오프셋 15, 실제 ACPI 스펙이 버전을 구분하는 방식 그대로)로 ACPI 1.0(`rsdt_address`, 32비트)과 2.0+(`xsdt_address`, 64비트)를 가른다 — 이번 단계도 QEMU/SeaBIOS 환경에서는 `63`이 확인한 대로 ACPI 1.0(리비전 0)만 오므로 RSDT 경로만 실제로 검증됐다:

```c
if (rsdp->revision >= 2U) {
    acpi_scan_tables(acpi_table_at(rsdp->xsdt_address), 1);
} else {
    acpi_scan_tables(acpi_table_at((u64)rsdp->rsdt_address), 0);
}
```

RSDT/XSDT는 SDT 헤더(36바이트) 뒤에 다른 테이블들의 물리주소 배열(각각 4바이트/8바이트)이 온다. 이 물리주소들은 QEMU 환경에서 전부 저메모리(< 1MB대)에 있어 `phys + KERNEL_OFFSET`로 바로 역참조할 수 있다 — `paging_init`이 이미 0~1GB를 식별 매핑해뒀기 때문에 이 범위에서는 새 매핑이 전혀 필요 없다(반면 IOAPIC/Local APIC MMIO 레지스터 자체는 물리주소가 4GB 근방이라 별도 매핑이 필요하다 — 5번 참고).

시그니처가 `"APIC"`인 테이블(MADT, Multiple APIC Description Table)을 찾으면 그 안의 가변 길이 엔트리들을 순회한다. 이번 단계에서 실제로 소비하는 엔트리 타입:

| Type | 의미 | 뽑아내는 값 |
|------|------|-------------|
| 0 | Processor Local APIC | `flags & 1`(enabled)인 첫 엔트리의 `apic_id` → BSP APIC ID |
| 1 | I/O APIC | `ioapic_id`, `ioapic_address`, `gsi_base` |
| 2 | Interrupt Source Override | ISA IRQ → GSI 재매핑 + 극성/트리거 플래그 |
| 5 | Local APIC Address Override | `LocalApicAddress` 필드(32비트)를 64비트 주소로 덮어씀 |

Type 2(Interrupt Source Override)가 실무적으로 중요하다 — 실제 ACPI 펌웨어는 흔히 "ISA IRQ0(PIT)은 IOAPIC 핀 0이 아니라 핀 2에 연결돼 있다"는 override를 보낸다. 이걸 무시하고 그냥 IRQ 번호를 GSI로 취급해 IOAPIC 핀 0을 열면 타이머 인터럽트가 영영 안 들어온다. `acpi_irq_to_gsi(u8 isa_irq)`가 이 재매핑 테이블을 인덱싱해 돌려주고, override가 없는 IRQ는 `irq_to_gsi[i] = i`(항등 매핑)로 초기화돼 있다. 이번 단계가 실제로 QEMU에서 관찰한 값은 override 없음(`gsi_base=0`이고 IRQ0/IRQ1이 그대로 GSI0/GSI1) — 그래도 override 파싱 코드 자체는 실제 리눅스(`mp_override_legacy_irq`)와 같은 방식으로 일반적으로 짜뒀다.

RSDP/SDT 체크섬 검증은 하지 않는다 — GRUB이 넘겨준 테이블이 항상 유효하다고 가정하는, 이 프로젝트가 QEMU 전용이라는 전제 위의 의도적 생략이다.

## 2) `boot/apic.c`/`.h`: Local APIC — x2APIC(MSR) 기본, xAPIC(MMIO) 폴백

```c
cpuid(1U, &a, &b, &c, &d);
use_x2apic = (c & (1U << 21)) ? 1 : 0;

if (use_x2apic) {
    u64 base = rdmsr(IA32_APIC_BASE_MSR);
    wrmsr(IA32_APIC_BASE_MSR, base | (1ULL << 10) | (1ULL << 11));
} else {
    lapic_mmio = map_mmio(LAPIC_MMIO_VADDR, acpi_lapic_address());
}
```

`CPUID.01H:ECX[21]`이 x2APIC 지원 여부다. 지원하면 `IA32_APIC_BASE`(MSR 0x1B)의 `EXTD`(비트10)를 켜서 x2APIC 모드로 전환하고, 이후 모든 Local APIC 레지스터 접근은 MSR(`0x800 + xAPIC오프셋/16`, SDM이 정의한 그대로의 변환식)로 간다. 미지원이면 `acpi_lapic_address()`(기본 `0xFEE00000`, MADT type5 override가 있으면 그 값)를 MMIO로 매핑해 옛날 방식(`0xF0`=SVR, `0xB0`=EOI, `0x20`=ID 오프셋)으로 접근한다. `lapic_read`/`lapic_write`가 이 두 경로를 감싸 상위 코드(`apic_id`, `apic_eoi`)는 어느 모드인지 몰라도 되게 했다 — 단, x2APIC의 ID 레지스터는 32비트 값을 그대로 담고 xAPIC MMIO ID 레지스터는 상위 8비트에만 담는 차이가 있어 `apic_id()`에서 그 부분만 분기한다.

**QEMU/TCG 환경의 한계**: `-cpu qemu64,+x2apic`로 CPU에 플래그를 켜도, 이 프로젝트가 쓰는 TCG(소프트웨어 에뮬레이션, WSL2 안에 KVM 없음)는 x2APIC CPUID 비트 자체를 지원하지 않는다:

```
qemu-system-x86_64: warning: TCG doesn't support requested feature: CPUID.01H:ECX.x2apic [bit 21]
```

그 결과 `CPUID.01H:ECX[21]`이 항상 0으로 관측되고, 코드의 게이팅 로직이 정확히 의도한 대로 xAPIC MMIO 폴백을 탄다 — **이번 단계는 xAPIC MMIO 경로만 실제로 검증됐고, x2APIC MSR 경로는 KVM(`-enable-kvm`)이나 실제 하드웨어에서만 확인 가능하다.** `-cpu qemu64,+x2apic` 플래그 자체는 의도를 남겨두기 위해 유지한다(실제 CPU/KVM에서 실행하면 이 플래그가 의미를 가지며 x2APIC 경로가 켜진다).

## 3) `boot/apic.c`: I/O APIC — 항상 MMIO, 리다이렉션 테이블 프로그래밍

```c
ioapic_mmio = map_mmio(IOAPIC_MMIO_VADDR, acpi_ioapic_address());
ver = ioapic_read(IOAPIC_REG_VER);
max_entry = (ver >> 16) & 0xFFU;
for (i = 0U; i <= max_entry; i++) {
    ioapic_write(REDTBL + i*2,     IOAPIC_REDTBL_MASKED);
    ioapic_write(REDTBL + i*2 + 1, 0U);
}
```

IOAPIC은 `IOREGSEL`(오프셋0x00)에 레지스터 인덱스를 쓰고 `IOWIN`(오프셋0x10)으로 그 레지스터를 읽고 쓰는 간접 접근 방식이다. 버전 레지스터(인덱스1)의 상위 바이트가 "최대 리다이렉션 엔트리 번호"라 이 값으로 실제 핀 개수(QEMU 기본 24개)를 알아내 전부 마스크한 채로 초기화한다.

`ioapic_unmask_irq(u8 irq, u8 vector)`가 `interrupts_unmask_irq`(아래 4번)에서 호출되는 실제 라우팅 지점이다 — ISA IRQ를 `acpi_irq_to_gsi`로 GSI로 바꾸고, `gsi - ioapic_gsi_base`로 이 IOAPIC 안에서의 로컬 핀 번호를 구해 리다이렉션 테이블 엔트리(64비트, 32비트 레지스터 2개)를 채운다:

- 하위 32비트: 벡터 번호 + (MADT override 극성/트리거 플래그가 있으면 active-low/level-triggered 비트)
- 상위 32비트: 목적지 필드(비트24~31) = 현재 `apic_id()` — 지금은 코어가 하나뿐이라 항상 BSP

## 4) `boot/interrupts.c`: 8259 PIC를 안전하게 죽이기

`pic_remap(0x20, 0x28)`은 그대로 남아있다 — 다만 이제 이 함수의 존재 이유가 바뀌었다: PIC를 실제로 쓰기 위한 초기화가 아니라, 혹시 모를 스퓨리어스 신호가 CPU 예외 벡터(0~31)와 겹치지 않게 치워둔 뒤 두 컨트롤러를 완전히 마스크(`0xFF`)해서 다시는 건드리지 않기 위한 안전장치다. 이건 실제 리눅스가 IOAPIC을 쓸 때도 레거시 8259를 남겨두고 전부 마스크하는 것과 같은 관례다. `pic_unmask_irq`/`pic_write_masks`/`pic_send_eoi`는 이제 아무도 안 부르므로 완전히 삭제했다.

`handle_irq`의 EOI가 `pic_send_eoi(irq)` → `apic_eoi()`로 바뀌었고, `interrupts_unmask_irq(u8 irq)`(시그니처는 그대로라 `timer.c`/`keyboard.c` 호출부는 무변경)의 내부 구현이 `pic_unmask_irq(irq)` → `ioapic_unmask_irq(irq, 0x20+irq)`로 바뀌었다 — 벡터 번호 체계(`0x20`=IRQ0, `0x28`=슬레이브 오프셋 표기)는 그대로 유지해 IDT 쪽 배선은 한 줄도 안 건드렸다.

## 5) MMIO 가상주소 배치: `KERNEL_OFFSET + paddr` 트릭이 여기선 안 통한다

`paging.c`의 기존 `tbl(phys) = phys + KERNEL_OFFSET`는 0~1GB 물리 RAM을 위해 만들어진 관례이고, 이 범위 안에서만 안전하다. IOAPIC(`0xFEC00000`)과 xAPIC MMIO(`0xFEE00000`)에 그대로 이 공식을 적용했다가 실제로 삼단 부팅 도중 페이지 폴트가 났다:

```
KERNEL_OFFSET(0xFFFFFFFF80000000) + 0xFEE00000
  = 하위 32비트: 0x80000000 + 0xFEE00000 = 캐리 발생, 결과 0x7EE00000
  = 캐리가 상위 32비트(0xFFFFFFFF)를 오버플로시켜 0x00000000으로 wrap
  → 64비트 합 = 0x000000007EE00000  (의도한 상위 캐노니컬 주소가 아니라 낮은 32비트 주소!)
```

`KERNEL_OFFSET`의 하위 32비트가 이미 `0x80000000`이라, 여기에 `0x80000000` 이상인 물리주소(딱 IOAPIC/LAPIC의 4GB 근방 주소가 여기 해당)를 더하면 반드시 64비트 전체가 오버플로해서 낮은 주소로 wrap된다. 실제로 `apic_eoi()`가 이 wrap된 주소(`0x7EE000B0`)에 접근해 매핑되지 않은 페이지에 쓰다 폴트가 났다.

고친 방법은 실제 OS의 `ioremap()`과 같다 — 물리주소에서 가상주소를 유도하지 않고, 커널 전용의 **임의 가상주소 창**을 하나 정해서 거기에 원하는 물리 프레임을 매핑한다:

```c
#define LAPIC_MMIO_VADDR  (KERNEL_OFFSET + 0x41000000ULL)
#define IOAPIC_MMIO_VADDR (KERNEL_OFFSET + 0x41001000ULL)
```

`KHEAP_MAX`(`KERNEL_OFFSET + 0x40800000`)보다 위, 8MB 이상 떨어진 별도 페이지 두 개다. `paging.c`에 새로 추가한 `page_map_mmio(vaddr, paddr)`가 이 매핑을 실제로 건다 — `page_map_frame`과 동일한 4KB 페이지 워크지만, PTE에 `PCD`(Page Cache Disable, 비트4)를 추가로 세팅한다. MMIO 레지스터를 일반 RAM처럼 캐시 가능 상태로 매핑하면 이론적으로 읽기/쓰기가 캐시에 머물고 실제 디바이스 레지스터에 도달하지 않을 수 있다 — 이 프로젝트가 타겟으로 하는 QEMU/TCG는 명령어 단위로 항상 에뮬레이션하므로 실제로는 이 비트가 없어도 동작하지만, 실제 하드웨어/KVM에서도 맞게 동작하도록 원칙대로 세팅해뒀다.

## 검증

`make clean && make run-nogui` 부팅 배너(관련 구간만, `63`과 나란히):

```
acpi: RSDP tag received (ACPI 1.0, 20 bytes) at 0xFFFFFFFF80100560
acpi: MADT parsed (bsp apic id=0, ioapic id=0 base=0xFEC00000 gsi_base=0)
IDT ready: 256 entries (16-byte gates) PIC=0x20/0x28
apic: xAPIC (MMIO) enabled, local apic id=0
ioapic: base=0xFEC00000 gsi_base=0 entries=24 masked
heap: kernel dir adopted, window at 0xFFFFFFFFC0400000 (mapped=128MB)
...
timer: PIT 100Hz IRQ0 ready
keyboard ready: IRQ1 unmasked
processes: init spawned pid=0
```

**회귀**: `63-multiboot2`와 `64-apic`을 각각 `make clean && make run-nogui`로 새로 부팅해 배너 전체를 `diff`한 결과, 새로 추가된 `acpi: MADT parsed`/`apic:`/`ioapic:` 세 줄을 빼면 **완전히 동일**하다 — `51`~`62`의 ext2/getdents/심링크/PATH exec/파이프/리다이렉션/`fcntl` 검증(`mkdir`/`symlink`/`unlink`/`ls`/`redirect`/`busybox sh` 전부 포함, 200줄 넘는 로그)이 한 글자도 다르지 않게 통과했다. 이건 타이머(IRQ0)/키보드(IRQ1) 인터럽트가 IOAPIC 경유로도 예전과 동일한 타이밍·순서로 스케줄러와 셸을 굴렸다는 뜻이다 — PIC에서 IOAPIC으로의 전환이 기존에 인터럽트에 의존하던 모든 서브시스템(스케줄러의 sleep/wakeup, 키보드 드라이버)에 아무 부작용을 안 남겼다는 확인이다. `e2fsck -f -n build/disk.img`도 에러 없이 통과했고, `grub-file --is-x86-multiboot2 build/kernel.elf`도 exit 0이다.

## 완료 기준

`make clean && make run-nogui`에서 부팅 배너가 위 "검증" 절의 `acpi: MADT parsed`/`apic:`/`ioapic:` 세 줄을 포함해야 하고, 그 이후 51~62의 모든 출력이 63과 (새 줄들을 제외하면) 동일해야 한다. `e2fsck -f -n build/disk.img`가 에러 없이 통과해야 한다. `grub-file --is-x86-multiboot2 build/kernel.elf`가 exit 0이어야 한다.

## 이전 단계(63) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/acpi.c`, `boot/acpi.h` | 신규 | RSDP(리비전 필드로 1.0/2.0+ 판별) → RSDT/XSDT 순회 → MADT(`"APIC"`) 파싱. Local APIC(type0, BSP id)/I-O APIC(type1)/Interrupt Source Override(type2)/Local APIC Address Override(type5) 엔트리 처리, ISA IRQ→GSI 재매핑+극성/트리거 플래그 테이블 |
| `boot/apic.c`, `boot/apic.h` | 신규 | CPUID(이 프로젝트 최초 도입)로 x2APIC 지원 게이팅, x2APIC(MSR)/xAPIC(MMIO) 겸용 Local APIC 드라이버(`apic_init`/`apic_id`/`apic_eoi`), I/O APIC 드라이버(`ioapic_init`/`ioapic_unmask_irq`) |
| `boot/paging.c`, `boot/paging.h` | 수정 | `page_map_mmio(vaddr, paddr)` 추가 — `page_map_frame`과 동일한 4KB 페이지 워크에 `PTE_PCD`(캐시 비활성화) 추가 |
| `boot/interrupts.c` | 수정 | `pic_unmask_irq`/`pic_write_masks`/`pic_send_eoi`/`pic1_mask`/`pic2_mask` 삭제(더는 아무도 안 씀); `pic_remap`은 이제 "리맵 후 완전 마스크"로 용도가 바뀜(레거시 PIC 영구 비활성화); `handle_irq`의 EOI가 `apic_eoi()`로, `interrupts_unmask_irq`의 내부가 `ioapic_unmask_irq`로 교체(공개 시그니처는 무변경이라 `timer.c`/`keyboard.c` 호출부는 그대로) |
| `boot/kernel.c` | 수정 | `acpi_init()`(RSDP 파싱 후 호출, MADT/IOAPIC 못 찾으면 fatal) + `apic_init()` + `ioapic_init()` 호출과 로그 라인 추가, `interrupts_init()`와 `kheap_init()` 사이에 배치 |
| `Makefile` | 수정 | `boot/acpi.c`/`boot/apic.c` 빌드 규칙과 `KERNELELF` 링크 목록에 추가; QEMU 실행 타겟에 `-cpu qemu64,+x2apic` 추가(TCG 제약으로 실제로는 xAPIC 폴백만 검증됨, 5번 참고) |
| 나머지 전부 | 변경 없음 | 63의 파일 그대로 |

## 다음 단계 힌트

- **x2APIC MSR 경로는 이 저장소의 QEMU/TCG 환경에서 검증 불가능한 채로 남는다**: 코드 게이팅 로직 자체는 실제 리눅스와 같은 방식(CPUID 확인 → 지원 시 즉시 x2APIC 전환)으로 짜여 있지만, TCG가 `CPUID.01H:ECX[21]`을 절대 세팅해주지 않아 항상 xAPIC MMIO 폴백만 탄다. KVM(`-enable-kvm`, 리눅스 호스트 + `/dev/kvm` 필요, 이 프로젝트의 WSL2 환경엔 없음)이나 실제 하드웨어에서만 그 경로가 실행된다 — 이 프로젝트 로드맵 안에서 검증할 계획은 없다.
- **Local APIC 타이머는 안 건드렸다**: PIT(`15-pit-timer`)를 그대로 IRQ0 소스로 쓴다. Local APIC 자체 내장 타이머(TSC-deadline 등)로 옮기는 건 이번 단계 범위 밖이다.
- **IPI(Inter-Processor Interrupt)는 구현하지 않았다**: `ICR`(Interrupt Command Register) 레지스터를 아예 안 건드렸다 — 지금 커널 스레드는 전부 소프트웨어 스케줄링(단일 코어)이라 필요 없다. SMP를 붙이는 로드맵이 생기면 그때 `apic.c`에 추가.
- **`65-pcie-enum`으로 이동**: MADT까지 파싱해 IOAPIC/LAPIC 기반을 갖췄으니, 로드맵상 다음은 PCIe 버스 스캔(legacy config space I/O 포트)이다.
