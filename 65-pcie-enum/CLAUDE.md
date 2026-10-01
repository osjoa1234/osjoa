# 65 — pcie-enum

**목표**: `64-apic`이 확보한 IOAPIC/Local APIC 기반 위에, PCI/PCIe 장치를 실제로 나열한다. config space 접근은 **ECAM(Enhanced Configuration Access Mechanism, MMIO) 전용**이다 — legacy I/O 포트(`CONFIG_ADDRESS`/`CONFIG_DATA`, `0xCF8`/`0xCFC`)는 이 커널에 아예 없다. bus × device 0~31 × function 0~7 전체 조합(BDF)을 순회하면서, vendor ID가 유효한(0xFFFF가 아닌) 자리마다 device ID/class code/BAR/capability list를 읽어 로그로 찍는다.

ECAM은 PCIe 스펙이 config space를 함수당 256바이트에서 4096바이트로 확장하면서 생긴 접근 방식인데, 그 확장된 0x100~0xFFF 구간은 애초에 MMIO로만 닿을 수 있다. legacy 포트 I/O는 PCIe 이전 세대 칩셋의 유물이고, ECAM 베이스 주소만 있으면 필요한 모든 필드(vendor/device/class/BAR/capability list, MSI-X 캡 포함)를 그대로 읽을 수 있어서 이번 커널은 legacy 경로를 아예 구현하지 않았다 — ECAM 베이스는 ACPI `MCFG` 테이블에서 얻고, 그 테이블이 없으면(즉 이 칩셋에 PCIe 루트 컴플렉스가 없으면) `64-apic`이 MADT/IOAPIC 못 찾았을 때와 똑같이 `halt_forever()`로 멈춘다.

## 0) QEMU 머신을 q35로 고정한 이유

ECAM은 PCIe 네이티브 칩셋에만 있는 기능이라 ACPI `MCFG` 테이블 자체가 PCIe 루트 컴플렉스를 가진 칩셋에서만 나온다. QEMU 기본 머신(`pc`, i440FX 기반)은 PCIe 이전 세대 칩셋이라 `MCFG`를 안 만들어준다 — 그래서 이 커널은 `-M q35`(ICH9 기반) 위에서만 부팅할 수 있고, i440fx로 부팅하면 `acpi: no MCFG found, cannot continue`로 바로 멈춘다(아래 "검증" 절의 폴백 없음 확인 참고).

q35로 바꾸면 부작용이 하나 있다: q35의 기본 SATA 컨트롤러는 AHCI(ICH9, class `0x01/06`)라 `51-ata-pio`가 하드코딩한 legacy IDE 포트(`0x1F0`~`0x1F7`, `0x3F6`)에 응답하지 않는다. `-drive if=ide`로 붙이면 QEMU가 그 드라이브를 AHCI 쪽에 물려버려서 `ext2: superblock read failed`로 깨지는 걸 직접 확인했다(아래 "검증 중 발견한 회귀" 참고). 그래서 디스크는 `if=ide` 대신 PCI 장치로 legacy IDE 컨트롤러(`piix3-ide`)를 q35 위에 명시적으로 얹어서 붙인다:

```
-drive id=disk,file=build/disk.img,format=raw,if=none \
-device piix3-ide,id=ide -device ide-hd,drive=disk,bus=ide.0
```

이러면 `pci_scan`이 나열하는 장치 목록에 `piix3-ide`가 legacy PCI storage 장치(`8086:7010`, `class=0x01/01`, capability list 없음)로 그대로 잡히고, ATA 드라이버는 예전처럼 `0x1F0` 포트로 응답을 받는다. q35의 AHCI 컨트롤러(`00:1F.2`) 자체를 드라이버로 붙이는 건 이번 단계 범위 밖이다 — `51-ata-pio`가 이미 살아있는 legacy IDE 경로고, AHCI는 다루지 않기로 로드맵에 적어뒀던 그대로다(`66`~`67`에서 이 자리를 NVMe로 대체할 예정). **이건 PCI config space 접근 방식(ECAM 전용)과는 다른 얘기다** — ATA 디스크가 쓰는 legacy I/O 포트(`0x1F0` 등)는 장치 자신의 BAR가 가리키는 포트고, config space 자체를 읽는 방식(ECAM)과는 별개의 층이다.

## 1) `boot/acpi.c`/`.h`: `MCFG` 파싱

`64-apic`이 만든 `acpi_scan_tables`(RSDT/XSDT를 순회하며 시그니처로 SDT를 찾는 범용 함수)에 `"MCFG"` 분기 하나를 추가했을 뿐, RSDT/XSDT 순회 자체는 전혀 새로 만들지 않았다:

```c
static void acpi_scan_tables(const struct acpi_sdt_header *root, int is_xsdt)
{
    ...
    if (acpi_sig_is(table, "APIC")) {
        madt_parse(table);
    } else if (acpi_sig_is(table, "MCFG")) {
        mcfg_parse(table);
    }
}
```

`MCFG` 테이블 레이아웃(SDT 헤더 36바이트 + 8바이트 예약 + 세그먼트 그룹당 16바이트 엔트리 배열):

| 오프셋 | 크기 | 필드 |
|---|---|---|
| 0 | 36 | SDT 헤더(시그니처 `"MCFG"` 포함) |
| 36 | 8 | 예약 |
| 44 | 8 | `base_address`: 이 세그먼트 그룹의 ECAM MMIO 베이스 물리주소 |
| 52 | 2 | `pci_segment_group` |
| 54 | 1 | `start_bus` |
| 55 | 1 | `end_bus` |
| 56 | 4 | 예약 |

세그먼트 그룹이 여러 개일 수 있지만(멀티 루트 컴플렉스 서버 등), QEMU는 세그먼트 그룹 0 하나만 주므로 `mcfg_parse`는 첫 엔트리만 읽는다. QEMU/SeaBIOS+q35 환경에서 실제 값은 `base_address=0xB0000000`, `start_bus=0`, `end_bus=255`였다.

`boot/kernel.c`는 `acpi_init()` 직후, 기존 MADT/IOAPIC 존재 확인과 나란히 `MCFG` 존재도 확인한다 — 못 찾으면 `64-apic`이 MADT 없을 때 하던 것과 같은 패턴으로 바로 멈춘다:

```c
if (!acpi_mcfg_found()) {
    console_set_color(0x0CU);
    console_printf("acpi: no MCFG found, cannot continue\n");
    halt_forever();
}
```

## 2) `boot/pci.c`: ECAM 주소 계산

ECAM의 물리주소 계산은 bus/device/function/offset이 그대로 물리주소의 비트 필드가 되는 단순한 구조다:

| 비트 | 의미 |
|------|------|
| 0~11 | 레지스터 오프셋(함수당 4096바이트 전체) |
| 12~14 | function |
| 15~19 | device |
| 20~27 | bus |
| 28~ | ECAM 베이스(`MCFG.base_address`) |

```c
static u32 pci_config_read32(u8 bus, u8 device, u8 function, u8 offset)
{
    u64 phys = g_ecam_base + ((u64)bus << 20) + ((u64)device << 15) + ((u64)function << 12);
    volatile u32 *window;

    page_map_mmio(PCI_ECAM_VADDR, (u32)phys);
    window = (volatile u32 *)PCI_ECAM_VADDR;

    return window[(offset & 0xFCU) / 4U];
}
```

함수 하나의 config space(4096바이트)가 정확히 페이지 하나(4096바이트) 크기라서, `apic.c`의 LAPIC/IOAPIC과 똑같은 "고정 가상주소 창 하나 + `page_map_mmio`로 물리 페이지만 갈아끼우기" 패턴을 그대로 재사용한다 — BDF 하나를 스캔할 때마다 `PCI_ECAM_VADDR`(`KERNEL_OFFSET + 0x41002000`, LAPIC `0x41000000`/IOAPIC `0x41001000` 바로 다음 슬롯)가 가리키는 물리 페이지를 그 BDF의 config space로 바꿔치기하고, 남은 필드는 그 안에서 포인터 산술로 읽는다. `pci_config_read16`/`8`은 이 함수 하나에만 의존한다.

`pci_scan()`은 `acpi_mcfg_base()`/`acpi_mcfg_end_bus()`로 스캔 범위를 정한다(QEMU 환경에선 `end_bus=255`).

## 3) 기존 스캔 절차 / BAR / capability list 순회는 무변경

BDF 전수조사(`pci_scan_function`/`pci_scan_device`), BAR 디코딩(MMIO32/64/IO 구분), capability list 순회 로직은 `pci_config_read32`가 ECAM으로 바뀐 것 외에는 손대지 않았다.

## 검증

`make clean && make run-nogui` 부팅 배너(관련 구간):

```
ioapic: base=0xFEC00000 gsi_base=0 entries=24 masked
pci: scanning configuration space via ECAM MMIO base=0x00000000B0000000 (bus 0-255)
    00:00.0 vendor=0x8086 device=0x29C0 class=0x06/00/00 (bridge)
    00:01.0 vendor=0x1234 device=0x1111 class=0x03/00/00 (display)
        BAR0: MMIO32 base=0xFD000000 prefetch=1
        BAR2: MMIO32 base=0xFEBD4000 prefetch=0
    00:02.0 vendor=0x8086 device=0x10D3 class=0x02/00/00 (network)
        BAR0: MMIO32 base=0xFEB80000 prefetch=0
        BAR1: MMIO32 base=0xFEBA0000 prefetch=0
        BAR2: I/O port base=0xC040
        BAR3: MMIO32 base=0xFEBD0000 prefetch=0
        cap 0x01: power management
        cap 0x05: MSI
        cap 0x10: PCI Express
        cap 0x11: MSI-X
    00:03.0 vendor=0x8086 device=0x7010 class=0x01/01/80 (storage)
        BAR4: I/O port base=0xC080
    00:1F.0 vendor=0x8086 device=0x2918 class=0x06/01/00 (bridge)
    00:1F.2 vendor=0x8086 device=0x2922 class=0x01/06/01 (storage)
        BAR4: I/O port base=0xC060
        BAR5: MMIO32 base=0xFEBD5000 prefetch=0
        cap 0x05: MSI
        cap 0x12: unknown
    00:1F.3 vendor=0x8086 device=0x2930 class=0x0C/05/00 (serial bus)
        BAR4: I/O port base=0x0700
pci: 7 device(s) found
heap: kernel dir adopted, window at 0xFFFFFFFFC0400000 (mapped=128MB)
ata: primary master ready (0x1F0-0x1F7, ctrl=0x3F6)
ext2: superblock magic=0xEF53 rev=1 block_size=1024 blocks=16384 inodes=4096
```

q35가 노출하는 장치: Q35 MCH host bridge(`8086:29C0`), Bochs/stdvga(`1234:1111`), e1000e NIC(`8086:10D3`, PCIe 네이티브라 capability list 4개(PM/MSI/PCI Express/MSI-X)를 실제로 갖는다), 명시적으로 붙인 legacy IDE(`8086:7010`, ATA 호환용), ICH9 LPC bridge(`8086:2918`), ICH9 AHCI(`8086:2922`, capability list로 MSI와 `0x12`(SATA capability, 이름 미등록이라 `unknown`)를 갖는다), ICH9 SMBus(`8086:2930`). **capability list 파싱 코드가 실제 capability를 가진 장치로 검증된 건 이번 단계가 처음이다** — 원래 i440fx 환경에선 어떤 장치도 capability list 비트를 켜고 있지 않아 `cap` 줄이 한 번도 안 찍혔었다.

`e2fsck -f -n build/disk.img`, `grub-file --is-x86-multiboot2 build/kernel.elf` 둘 다 exit 0.

### 폴백 없음 확인: `MCFG` 없는 머신은 그냥 멈춘다

`make run-nogui`가 쓰는 q35 대신 QEMU 기본 머신(i440fx, `MCFG` 없음)으로 직접 부팅해보면:

```
acpi: RSDP tag received (ACPI 1.0, 20 bytes) at 0xFFFFFFFF80100560
acpi: MADT parsed (bsp apic id=0, cpus=1, ioapic id=0 base=0xFEC00000 gsi_base=0)
acpi: no MCFG found, cannot continue
```

여기서 멈추고 더 진행하지 않는다 — legacy 포트 I/O로 조용히 대체되는 경로가 없다는 뜻이다. 이 커널은 ECAM이 없는 칩셋에서는 애초에 PCI를 스캔할 수 없다.

### 검증 중 발견한 회귀: q35 + `if=ide` 조합은 깨진다

머신을 q35로 바꾸면서 처음엔 디스크를 기존처럼 `-drive file=...,if=ide,index=0`로만 붙여봤는데, 그러면 QEMU가 그 드라이브를 AHCI 컨트롤러에 물려버려서 ATA 드라이버가 기대하는 `0x1F0` 포트엔 아무 장치도 없다:

```
ata: primary master ready (0x1F0-0x1F7, ctrl=0x3F6)
ext2: superblock read failed
ext2: init failed
...
shell: ext2 open /disk/hello.txt failed
```

`piix3-ide`를 PCI 장치로 명시적으로 붙이자(위 "0)" 참고) 정상화됐다 — 이게 위 "검증" 절의 최종 로그다. 이 문제는 **51-ata-pio가 이미 갖고 있던 전제조건(legacy IDE 포트가 살아있어야 한다)을 이번 단계가 깨뜨릴 뻔한 사례**라 여기 기록해둔다. 앞서 말했듯 이 IDE 포트 문제는 PCI config space 접근 방식(ECAM 전용)과는 무관한, 디스크 장치 자신의 I/O 포트 문제다.

## 이전 단계(64) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/pci.c`, `boot/pci.h` | 신규 | ECAM(MMIO) 전용 config space read8/16/32, bus/device/function 전수 스캔(`pci_scan`), BAR 디코딩(MMIO32/64/IO, prefetchable), capability list 순회, class code 이름 매핑 |
| `boot/acpi.c`, `boot/acpi.h` | 수정 | `acpi_scan_tables`에 `"MCFG"` 시그니처 분기 추가, `mcfg_parse` + `acpi_mcfg_found`/`acpi_mcfg_base`/`acpi_mcfg_start_bus`/`acpi_mcfg_end_bus` 접근자 신규 |
| `boot/kernel.c` | 수정 | `acpi_init()` 직후 `acpi_mcfg_found()` 확인(없으면 `halt_forever()`, MADT/IOAPIC 확인과 같은 패턴) + `pci_scan()` 호출 추가(`ioapic_init()`과 `kheap_init()` 사이) |
| `Makefile` | 수정 | `boot/pci.c` 빌드 규칙과 `KERNELELF` 링크 목록에 추가. `run`/`run-nogui`가 QEMU 머신을 `-M q35`로, 디스크 연결을 `if=ide` 한 줄에서 `piix3-ide`(PCI 장치) + `ide-hd` 명시적 attach로 교체 |
| 나머지 전부 | 변경 없음 | 64의 파일 그대로 |

## 완료 기준

`make clean && make run-nogui`에서 부팅 배너가 위 "검증" 절의 `pci:` 블록(ECAM 배너 + 7개 장치)을 포함해야 하고, `ata:`/`ext2:` 이후 51~64의 나머지 흐름이 정상 동작해야 한다. `e2fsck -f -n build/disk.img`가 에러 없이 통과해야 한다. `grub-file --is-x86-multiboot2 build/kernel.elf`가 exit 0이어야 한다.

## 다음 단계 힌트

- **BAR 크기 프로브(0xFFFFFFFF 쓰고 읽어서 크기 역산)는 구현하지 않았다**: 순수 열거 범위를 벗어나는 쓰기 동작이라 미뤘다 — 실제로 BAR가 가리키는 MMIO 영역을 매핑해 쓰는 `66-nvme-admin`에서 필요해지면 그때 추가한다.
- **capability list 안의 세부 데이터(MSI 메시지 주소/데이터, MSI-X 테이블/PBA 오프셋 등)는 아직 안 읽는다**: ID와 next pointer로 리스트만 순회했다 — MSI-X 세팅은 `68-msi-x`의 몫이다.
- **class code로 장치를 찾는 헬퍼(`pci_find_by_class`류)는 아직 없다**: 이번 단계는 전체 나열만 하고 끝났다 — NVMe(class `0x01/08`)를 실제로 찾아 드라이버를 초기화하는 건 `66-nvme-admin`에서 필요해지는 시점에 추가한다.
- **PCI-to-PCI 브릿지(header type 1) 뒤의 secondary bus 재귀 탐색은 하지 않는다**: 이번 스캔은 `MCFG`가 알려준 bus 범위를 이미 평평하게(flat) 전부 순회하므로 브릿지가 어떤 secondary bus를 여는지와 무관하게 그 뒤에 달린 장치도 어차피 스캔 범위 안에 들어온다. 브릿지 구조를 실제로 반영한 depth-first 탐색이 필요해지는 시점까지는 미룬다.
- **q35의 AHCI 컨트롤러(`00:1F.2`)를 위한 드라이버는 쓰지 않는다**: 지금은 `piix3-ide`로 legacy IDE 경로만 유지했다 — 로드맵상 `51-ata-pio`의 후속은 AHCI가 아니라 `66`~`67`의 NVMe라 AHCI 드라이버 자체를 쓸 계획이 없다. `piix3-ide`는 어디까지나 51~64 검증을 계속 살려두기 위한 장치이고, 66~67에서 NVMe 컨트롤러를 붙이고 나면 disk.img를 NVMe로 옮길지 IDE로 남길지는 그때 판단한다.
- **세그먼트 그룹은 0번만 읽는다**: QEMU가 세그먼트 그룹 하나만 주기 때문이다. 멀티 세그먼트가 필요해지는 로드맵 항목은 지금 없다.
- **legacy 포트 I/O 경로는 커널에 존재하지 않는다**: `MCFG`가 없는 칩셋(i440fx 등)에서는 `halt_forever()`로 멈춘다. 이 프로젝트가 타깃으로 삼는 QEMU 환경은 이후로도 계속 `-M q35`이므로, legacy 폴백을 되살려야 할 필요가 생기는 로드맵 항목은 지금 없다.
