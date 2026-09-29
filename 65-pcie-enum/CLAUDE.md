# 65 — pcie-enum

**목표**: `64-apic`이 확보한 IOAPIC/Local APIC 기반 위에, PCI/PCIe 장치를 실제로 나열한다. legacy config space I/O 포트(`CONFIG_ADDRESS`=`0xCF8`, `CONFIG_DATA`=`0xCFC`)로 bus 0~255 × device 0~31 × function 0~7 전체 조합(BDF)을 순회하면서, vendor ID가 유효한(0xFFFF가 아닌) 자리마다 device ID/class code/BAR/capability list를 읽어 로그로 찍는다. PCIe 장치도 config space 첫 256바이트는 PCI와 호환이라 이 legacy 메커니즘 하나로 충분하다 — extended config space(ECAM/MCFG)는 다루지 않는다.

## 1) `boot/pci.c`/`.h`: config space 접근

```c
static u32 pci_config_read32(u8 bus, u8 device, u8 function, u8 offset)
{
    u32 address = (1U << 31) | ((u32)bus << 16) | ((u32)device << 11) |
                  ((u32)function << 8) | (offset & 0xFCU);

    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}
```

`CONFIG_ADDRESS`에 담는 32비트 레이아웃:

| 비트 | 의미 |
|------|------|
| 0~1 | 항상 0 |
| 2~7 | 레지스터 오프셋(4바이트 정렬) |
| 8~10 | function |
| 11~15 | device |
| 16~23 | bus |
| 24~30 | 예약(0) |
| 31 | enable(항상 1) |

`CONFIG_ADDRESS`에 쓴 뒤 `CONFIG_DATA`(32비트 포트)를 읽고 쓰면 그 주소가 가리키는 config space 4바이트가 오간다. 이 포트는 PCI 스펙이 아니라 인텔 칩셋 관례지만 x86 전반의 사실상 표준이다. `pci_config_read16`/`pci_config_read8`은 32비트 read 결과에서 오프셋의 하위 비트로 시프트해 뽑아 쓴다(레지스터 자체는 항상 4바이트 정렬로만 접근 가능).

## 2) 스캔 절차: BDF 전수조사 + multi-function 판별

```c
static int pci_scan_function(u8 bus, u8 device, u8 function)
{
    u16 vendor_id = pci_config_read16(bus, device, function, PCI_OFF_VENDOR_ID);
    if (vendor_id == PCI_VENDOR_INVALID) return 0;
    ...
}

static void pci_scan_device(u8 bus, u8 device)
{
    if (!pci_scan_function(bus, device, 0U)) return;

    header_type = pci_config_read8(bus, device, 0U, PCI_OFF_HEADER_TYPE);
    if (!(header_type & PCI_HEADER_MULTIFUNC)) return;

    for (function = 1U; function < 8U; function++) {
        pci_scan_function(bus, device, function);
    }
}
```

vendor ID가 `0xFFFF`면 그 자리엔 장치가 없다는 뜻이다(PCI-SIG가 벤더ID로 절대 발급하지 않는 값이라, root complex가 빈 슬롯에 항상 이 값을 돌려주기로 정해뒀다). function 0의 vendor ID가 유효할 때만 header type(오프셋 `0x0E`)의 비트 7(multi-function 여부)을 확인해 function 1~7을 마저 본다 — 비트 7이 꺼져 있으면 이 장치는 function 0 하나뿐이다.

bus 0~255, device 0~31 전체를 매번 부팅 시 한 번씩 뒤지지만(최악 65536개 조합), 한 번 읽는 데 포트 I/O 두 번(`CONFIG_ADDRESS` 쓰기 + `CONFIG_DATA` 읽기)뿐이라 실제로는 QEMU에서 감지되지 않을 정도로 빠르다.

## 3) BAR 디코딩: MMIO vs I/O, 32비트 vs 64비트

```c
if (bar & 1U) {
    // I/O 공간: 비트0=1, 비트2~31이 베이스 포트
} else {
    u32 type = (bar >> 1U) & 0x3U;        // 00=32비트, 10=64비트
    u32 prefetchable = (bar >> 3U) & 0x1U;
    if (type == 2U) {
        // 다음 BAR와 합쳐 64비트 주소 하나
    }
}
```

BAR 비트 레이아웃 정리:

| BAR 종류 | 비트0 | 비트1~2 | 비트3 | 비트4(MMIO)/2(I/O)~31 |
|---|---|---|---|---|
| MMIO | 0 | 주소 폭(00=32비트, 10=64비트) | prefetchable | 베이스 주소(4바이트 정렬) |
| I/O | 1 | (비트1 예약) | — | 베이스 포트 주소(x86은 16비트뿐이라 상위는 0) |

64비트 MMIO BAR는 짝을 이루는 다음 BAR 레지스터가 상위 32비트를 담으므로, 이번 스캔은 그 다음 BAR 인덱스를 건너뛴다(같은 BAR를 두 번 별개로 찍지 않기 위해). **BAR가 실제로 매핑하는 영역의 크기(0xFFFFFFFF를 써봤다가 원복하는 프로브)는 이번 단계에서 하지 않는다** — 크기 프로브는 장치에 쓰기(write)를 가하는 동작이라 순수 열거(enumeration) 범위를 벗어난다고 보고, 실제로 그 크기가 필요해지는 시점(66-nvme-admin에서 BAR0 MMIO 창을 매핑할 때)으로 미룬다.

## 4) capability list 순회

```c
if (status & PCI_STATUS_CAP_LIST) {
    pointer = pci_config_read8(..., PCI_OFF_CAP_POINTER) & 0xFCU;
    while (pointer != 0U) {
        cap_id = pci_config_read8(..., pointer);
        next   = pci_config_read8(..., pointer + 1U);
        pointer = next & 0xFCU;
    }
}
```

status 레지스터(오프셋 `0x06`) 비트4가 켜져 있어야 capabilities pointer(오프셋 `0x34`)가 유효하다. 이 포인터부터 시작해 각 capability 구조체의 두 번째 바이트(next pointer)가 0이 될 때까지 연결 리스트를 따라간다. 알려진 ID만 이름을 붙이고(`0x01`=Power Management, `0x05`=MSI, `0x10`=PCI Express, `0x11`=MSI-X) 나머지는 `unknown`으로 찍는다 — MSI-X 자체를 세팅하는 건 `68-msi-x`의 몫이라 지금은 나열만 한다.

## 검증

`make clean && make run-nogui` 부팅 배너(관련 구간, `64`와 나란히):

```
ioapic: base=0xFEC00000 gsi_base=0 entries=24 masked
pci: scanning configuration space via 0xCF8/0xCFC
    00:00.0 vendor=0x8086 device=0x1237 class=0x06/00/00 (bridge)
    00:01.0 vendor=0x8086 device=0x7000 class=0x06/01/00 (bridge)
    00:01.1 vendor=0x8086 device=0x7010 class=0x01/01/80 (storage)
        BAR4: I/O port base=0xC040
    00:01.3 vendor=0x8086 device=0x7113 class=0x06/80/00 (bridge)
    00:02.0 vendor=0x1234 device=0x1111 class=0x03/00/00 (display)
        BAR0: MMIO32 base=0xFD000000 prefetch=1
        BAR2: MMIO32 base=0xFEBB0000 prefetch=0
    00:03.0 vendor=0x8086 device=0x100E class=0x02/00/00 (network)
        BAR0: MMIO32 base=0xFEB80000 prefetch=0
        BAR1: I/O port base=0xC000
pci: 6 device(s) found
heap: kernel dir adopted, window at 0xFFFFFFFFC0400000 (mapped=128MB)
```

QEMU 기본 i440fx 머신이 노출하는 6개 장치가 그대로 잡힌다: host bridge(`8086:1237`), PIIX3 ISA bridge(`8086:7000`), PIIX3 IDE 컨트롤러(`8086:7010`, class `0x01/01` storage, BAR4가 legacy IDE의 bus-master I/O 포트), PIIX4 ACPI/PM bridge(`8086:7113`), Bochs/stdvga 디스플레이(`1234:1111`, BAR0가 prefetchable 프레임버퍼, BAR2가 MMIO 레지스터), e1000 NIC(`8086:100E`, BAR0 MMIO + BAR1 I/O). 이 중 어느 장치도 status의 capability list 비트를 켜고 있지 않아 `cap` 줄은 한 번도 안 찍혔다 — QEMU 기본 머신의 레거시 칩셋/구형 장치 모델이 capability를 노출하지 않기 때문이며, 이건 `66`~`68`에서 NVMe 컨트롤러(PCIe native 장치라 반드시 capability list를 가짐)를 붙였을 때와 대조된다.

**회귀**: `64-apic`과 `65-pcie-enum`을 각각 `make clean && make run-nogui`로 새로 부팅해 배너 전체를 `diff`한 결과, 새로 추가된 `pci:` 블록(위 12줄)을 빼면 **완전히 동일**하다 — 커널 이미지가 `pci.o`만큼 커져 ISO 섹터 수가 1 늘고 그만큼 `phys mem: 32557` → `32556`(1페이지) 줄어든 차이만 있고, `51`~`62`의 ext2/getdents/심링크/PATH exec/파이프/리다이렉션/`fcntl` 검증은 한 글자도 다르지 않다. `e2fsck -f -n build/disk.img`도 에러 없이 통과했고, `grub-file --is-x86-multiboot2 build/kernel.elf`도 exit 0이다.

## 완료 기준

`make clean && make run-nogui`에서 부팅 배너가 위 "검증" 절의 `pci:` 블록을 포함해야 하고, 그 이후 51~64의 모든 출력이 64와 (새 블록을 제외하면) 동일해야 한다. `e2fsck -f -n build/disk.img`가 에러 없이 통과해야 한다. `grub-file --is-x86-multiboot2 build/kernel.elf`가 exit 0이어야 한다.

## 이전 단계(64) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/pci.c`, `boot/pci.h` | 신규 | `CONFIG_ADDRESS`(0xCF8)/`CONFIG_DATA`(0xCFC) 포트 I/O 기반 config space read8/16/32, bus/device/function 전수 스캔(`pci_scan`), BAR 디코딩(MMIO32/64/IO, prefetchable), capability list 순회, class code 이름 매핑 |
| `boot/kernel.c` | 수정 | `pci_scan()` 호출 추가 — `ioapic_init()`과 `kheap_init()` 사이에 배치(힙이 필요 없는 순수 열거라 힙 초기화보다 먼저 실행 가능) |
| `Makefile` | 수정 | `boot/pci.c` 빌드 규칙과 `KERNELELF` 링크 목록에 추가 |
| 나머지 전부 | 변경 없음 | 64의 파일 그대로 |

## 다음 단계 힌트

- **BAR 크기 프로브(0xFFFFFFFF 쓰고 읽어서 크기 역산)는 구현하지 않았다**: 순수 열거 범위를 벗어나는 쓰기 동작이라 미뤘다 — 실제로 BAR가 가리키는 MMIO 영역을 매핑해 쓰는 `66-nvme-admin`에서 필요해지면 그때 추가한다.
- **capability list 안의 세부 데이터(MSI 메시지 주소/데이터, MSI-X 테이블/PBA 오프셋 등)는 아직 안 읽는다**: ID와 next pointer로 리스트만 순회했다 — MSI-X 세팅은 `68-msi-x`의 몫이다.
- **class code로 장치를 찾는 헬퍼(`pci_find_by_class`류)는 아직 없다**: 이번 단계는 전체 나열만 하고 끝났다 — NVMe(class `0x01/08`)를 실제로 찾아 드라이버를 초기화하는 건 `66-nvme-admin`에서 필요해지는 시점에 추가한다.
- **PCI-to-PCI 브릿지(header type 1) 뒤의 secondary bus 재귀 탐색은 하지 않는다**: 이번 스캔은 bus 0~255를 이미 전부 평평하게(flat) 순회하므로 브릿지가 어떤 secondary bus를 여는지와 무관하게 그 뒤에 달린 장치도 어차피 스캔 범위 안에 들어온다. 브릿지 구조를 실제로 반영한 depth-first 탐색이 필요해지는 시점(예: 여러 단 브릿지를 넘어야 접근되는 장치가 로드맵에 생기면)까지는 미룬다.
