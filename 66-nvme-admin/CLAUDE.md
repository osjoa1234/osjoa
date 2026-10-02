# 66 — nvme-admin

**목표**: `65-pcie-enum`이 나열만 하던 PCI 버스에서 NVMe 컨트롤러(class `0x01/08`)를 실제로 찾아 admin queue(SQ/CQ 한 쌍)를 초기화하고, Identify Controller/Identify Namespace 커맨드를 보내 응답을 받는다. 완료 확인은 전부 CQ phase bit polling이다 — 인터럽트(MSI-X)는 `68-msi-x`의 몫이고, I/O 큐 생성과 실제 블록 read/write는 `67-nvme-io`의 몫이다. 이 단계는 "컨트롤러가 살아있고 admin 채널로 말이 통한다"까지만 검증한다.

## 0) `boot/pci.c`/`.h`: config space **쓰기**와 class 검색을 처음 추가

`65-pcie-enum`은 ECAM config space를 **읽기만** 했다 — 장치를 나열하기만 했지 건드린 적이 없다. 이번 단계에서 처음으로 쓰기가 필요해졌다:

```c
static void pci_config_write32(u8 bus, u8 device, u8 function, u8 offset, u32 value)
{
    u64 phys = g_ecam_base + ((u64)bus << 20) + ((u64)device << 15) + ((u64)function << 12);
    volatile u32 *window;

    page_map_mmio(PCI_ECAM_VADDR, (u32)phys);
    window = (volatile u32 *)PCI_ECAM_VADDR;

    window[(offset & 0xFCU) / 4U] = value;
}
```

`pci_config_read32`가 쓰던 "고정 가상주소 창 + `page_map_mmio`로 물리 페이지만 갈아끼우기" 패턴을 그대로 재사용했다. `pci_config_write16`은 32비트 읽기-수정-쓰기로 구현했다(ECAM 자체가 4바이트 정렬 접근만 지원).

**`pci_find_by_class(class_code, subclass, ...)`**: `65`의 "다음 단계 힌트"에 "NVMe(class `0x01/08`)를 실제로 찾아 드라이버를 초기화하는 건 `66-nvme-admin`에서 필요해지는 시점에 추가한다"고 미뤄뒀던 항목이다. `pci_scan()`과 같은 bus/device/function 전수 순회를 하지만 로그를 찍는 대신 class/subclass가 일치하는 첫 BDF를 리턴한다.

**`pci_enable_device(bus, device, function)`**: **이번 단계에서 가장 중요한 추가**다. `65`는 장치를 "읽기만" 했지 활성화한 적이 없다. PCI Command 레지스터(offset `0x04`)의 두 비트를 켠다:

- bit 1 (Memory Space Enable): 이게 꺼져있으면 BAR0 MMIO 자체가 응답하지 않는다.
- bit 2 (Bus Master Enable): 이게 꺼져있으면 컨트롤러가 CQ에 완료 엔트리를 DMA로 쓸 수 없다 — SQ에 커맨드를 넣고 doorbell을 울려도 CQ의 phase bit이 영원히 안 뒤집힌다.

이걸 빠뜨리면 증상이 매우 헷갈린다 — 레지스터 읽기는 되는데(이미 BIOS/SeaBIOS가 부팅 과정에서 켜놨을 수도 있어서 잘 되는 것처럼 보이다가) CQ가 조용히 멈추는 식으로 디버깅이 어려워질 수 있어서, 발견한 적은 없지만 명시적으로 켜고 시작한다.

**`pci_bar_address(bus, device, function, bar_index)`**: `pci_scan_bars`가 로그 찍을 때 쓰던 MMIO64 디코딩(`type==2`면 다음 BAR가 상위 32비트)을 그대로 재사용해서, 이번엔 로그 대신 물리주소 `u64`를 리턴하는 버전으로 뽑아냈다. NVMe BAR0는 스펙상 항상 64비트 MMIO라 이 함수 하나로 충분하다.

## 1) BAR0 레지스터는 MMIO 매핑, admin SQ/CQ와 Identify 버퍼는 그냥 RAM

이번 단계에서 커널이 다루는 메모리가 두 종류로 확실히 갈린다 — 이 구분을 코드에서 명확히 지켰다:

- **CAP/CC/CSTS/AQA/ASQ/ACQ 레지스터(컨트롤러 자신)**: `apic.c`/`pci.c`가 쓰던 패턴 그대로 `page_map_mmio(고정_가상주소, BAR0_물리주소)`로 매핑한다. 다음 빈 슬롯인 `KERNEL_OFFSET+0x41003000`을 썼다(LAPIC `0x41000000`, IOAPIC `0x41001000`, PCI ECAM `0x41002000` 다음).
- **admin SQ/CQ 엔트리, Identify 결과 버퍼**: 이건 MMIO가 아니라 그냥 물리 RAM이다. `elf.c`/`process.c`/`signal.c`가 이미 쓰던 `(u8 *)((u64)frame + KERNEL_OFFSET)` 캐스팅 패턴을 그대로 썼다 — `page_alloc()`으로 받은 프레임은 이 커널이 물리메모리를 `KERNEL_OFFSET`에 direct-map 해놨기 때문에 별도 매핑 없이 바로 읽고 쓸 수 있다. 컨트롤러는 이 주소를 PCIe를 통해 DMA로 직접 읽고 쓴다 — CPU의 페이지테이블 매핑과는 무관한 별도 경로다.

이 두 메모리의 성격 차이(레지스터=디바이스 자체, 큐/버퍼=호스트 RAM을 디바이스가 DMA로 공유)를 혼동하면 안 된다는 걸 분명히 하려고 이 절을 따로 뒀다.

**doorbell이 레지스터 페이지 하나를 더 쓴다는 점도 주의했다**: CAP~ACQ는 BAR0 오프셋 `0x00~0x37`(첫 페이지)에 있지만, doorbell은 오프셋 `0x1000`부터(기본 stride=4바이트 기준 `0x1000~0x1008`)라 **두 번째 페이지**에 있다. `page_map_mmio`는 한 번에 딱 한 페이지만 매핑하므로, 레지스터 구조체용 한 페이지(`NVME_MMIO_VADDR → BAR0_phys`)와 doorbell용 한 페이지(`NVME_MMIO_VADDR+0x1000 → BAR0_phys+0x1000`) 둘 다 매핑했다.

## 2) admin queue 초기화 순서

137번 글(`.assets/docs/posts/137.md`)에 적은 11단계를 그대로 코드로 옮겼다:

```c
if (regs->cc & NVME_CC_EN) {
    regs->cc &= ~NVME_CC_EN;
    nvme_wait_csts(regs, 0U);
}

sq_phys = page_alloc();
cq_phys = page_alloc();

regs->aqa = (queue_entries - 1U) | ((queue_entries - 1U) << 16U);
regs->asq = (u64)sq_phys;
regs->acq = (u64)cq_phys;

cc = NVME_CC_EN | (6U << 16U) | (4U << 20U);
regs->cc = cc;

nvme_wait_csts(regs, 1U);
```

CAP에서 읽은 MQES(0-based, 그래서 `+1`)와 실제 쓰려는 큐 크기(64개) 중 작은 쪽을 쓴다 — QEMU의 nvme 장치는 MQES가 2048이라 이번엔 항상 64개 그대로 쓰였지만, 더 작은 하드웨어를 만나도 안전하게 clamp된다. `ata_wait_ready`(`ata.c`)가 이미 쓰던 "상한 있는 spin loop, 넘으면 -1" 패턴을 `nvme_wait_csts`에도 그대로 썼다 — `CC.EN` 끄고 `CSTS.RDY`가 0 될 때까지, 그리고 `CC.EN` 켜고 `CSTS.RDY`가 1 될 때까지 둘 다 이 함수 하나로 처리한다.

CC에 `IOSQES=6`(2^6=64바이트), `IOCQES=4`(2^4=16바이트)를 넣었는데, 이 필드는 스펙상 **I/O 큐를 만들 때만** 쓰인다 — admin 큐 자체의 엔트리 크기는 고정이라 이번 단계엔 쓰이지 않는다. `67-nvme-io`에서 I/O SQ/CQ를 만들 때 이 값이 그대로 유효하도록 표준값을 미리 넣어뒀다.

## 3) Identify 커맨드 조립과 phase bit polling

`nvme_identify()` 하나로 Identify Controller(`CNS=0x01`)와 Identify Namespace(`CNS=0x00, NSID=1`) 둘 다 처리한다 — 커맨드 조립, 제출, 완료 대기, doorbell ack까지 공통이고 `cns`/`nsid`/`cid`/`data_phys`만 다르다:

```c
sqe->cdw0  = NVME_OPC_IDENTIFY | (cid << 16);
sqe->nsid  = nsid;
sqe->prp1  = (u64)data_phys;
sqe->cdw10 = cns;

*sq_tail = (*sq_tail + 1U) % NVME_ADMIN_QUEUE_ENTRIES;
*nvme_doorbell(0U, stride) = *sq_tail;

while (((cqe->status >> 16U) & 1U) != *phase) {
    spins++;
    if (spins > NVME_WAIT_SPINS) return -1;
}

*cq_head = (*cq_head + 1U) % NVME_ADMIN_QUEUE_ENTRIES;
if (*cq_head == 0U) *phase ^= 1U;
*nvme_doorbell(1U, stride) = *cq_head;
```

phase bit 의미론은 137번 글에서 정리한 그대로 구현했다 — 같은 바퀴 동안 쓰는 CQ 엔트리는 전부 같은 phase 값을 공유하고, `cq_head`가 큐 끝을 넘어 0으로 돌아올 때만(`*cq_head == 0U`) `*phase`를 뒤집는다. 엔트리 하나 읽을 때마다 뒤집는 게 아니다.

**이번 단계에서 실제로 걸렸던 버그**: `struct nvme_cqe`의 필드에 처음엔 `volatile`을 안 붙였다. `-O2`가 켜진 상태라, 컴파일러가 "이 루프 안에서 `cqe->status`를 아무도 안 바꾸니 한 번만 읽어도 된다"고 판단해서 메모리 재읽기를 루프 밖으로 끌어올렸다(hoist) — 그 결과 컨트롤러가 DMA로 실제 값을 써도 CPU는 그 전에 레지스터에 캐시해둔 값만 계속 비교해서 Identify Controller 요청이 10,000,000번 스핀을 다 채우고 타임아웃(-1)으로 끝났다. 공교롭게도 그 다음 Identify Namespace 요청이(같은 `cq_head`를 다시 읽는 버그와 맞물려) 어쩌다 성공한 것처럼 보이는 로그가 나와서 처음엔 "되는 줄" 착각할 뻔했다 — `nvme: identify controller failed status=0xFFFFFFFF`가 찍히는 걸 보고서야 실제로는 첫 요청이 깨졌다는 걸 알았다. `struct nvme_cqe`의 네 필드 전부 `volatile u32`로 바꾸니 바로 해결됐다. **DMA로 다른 주체(디바이스)가 채워주는 메모리를 폴링할 땐 반드시 `volatile`을 붙여야 한다**는 걸 실제로 겪은 사례라 여기 남긴다.

## 4) Identify 결과 파싱

Identify Controller 결과(4096바이트 버퍼)에서 Serial Number(offset 4, 20바이트), Model Number(offset 24, 40바이트), Firmware Revision(offset 64, 8바이트)를 꺼냈다 — 전부 ASCII를 공백으로 패딩한 필드라 trailing space를 잘라내고 널 종단 문자열로 바꾸는 `nvme_copy_trim`을 하나 뒀다. Identify Namespace 결과에서는 NSZE(offset 0, 8바이트, 논리 블록 단위 네임스페이스 크기)만 꺼냈다 — `67-nvme-io`에서 LBA 범위를 검증할 때 이 값이 필요해진다.

## Makefile / QEMU

NVMe 전용 빈 디스크 이미지(`build/nvme.img`, 16MB raw, ext2 포맷 없음)를 새로 만들고 `-device nvme,serial=deadbeef,drive=nvme0`로 붙였다 — 기존 `piix3-ide` + `disk.img`(51~65가 쓰는 ext2용)는 그대로 둔 채로 **같은 VM에 두 번째 디스크 컨트롤러를 추가**하는 형태다. QEMU 8.2.2에서 `-device nvme`를 지원하는 걸 확인했다(`qemu-system-x86_64 -device help | grep nvme`).

## 검증

`make clean && make run-nogui` 부팅 배너(관련 구간):

```
pci: 8 device(s) found
    ...
    00:04.0 vendor=0x1B36 device=0x0010 class=0x01/08/02 (storage)
        BAR0: MMIO64 base=0x00000000FEBD4000 prefetch=0
        cap 0x11: MSI-X
        cap 0x10: PCI Express
        cap 0x01: power management
    ...
nvme: found controller at 00:04.0
nvme: BAR0 phys=0x00000000FEBD4000 mqes=2048 dstrd=0 stride=4
nvme: admin queue ready (64 entries)
nvme: identify controller model="QEMU NVMe Ctrl" serial="deadbeef" fw="8.2.2"
nvme: identify namespace nsid=1 nsze=32768 blocks
heap: kernel dir adopted, window at 0xFFFFFFFFC0400000 (mapped=128MB)
ata: primary master ready (0x1F0-0x1F7, ctrl=0x3F6)
ext2: superblock magic=0xEF53 rev=1 block_size=1024 blocks=16384 inodes=4096
...
shell: ext2 /disk/hello.txt: hello ext2 root fs
```

`nsze=32768 blocks`는 512바이트 LBA 기준 32768×512=16MB로, `nvme.img` 크기와 정확히 일치한다(QEMU nvme 장치는 기본 LBA 크기를 512바이트로 노출한다).

`00:04.0`이 QEMU의 가상 NVMe 컨트롤러다(vendor `0x1B36`=Red Hat, device `0x0010`). PCIe 네이티브 장치답게 capability list에 MSI-X/PCI Express/전원관리 셋을 갖는다.

51~65가 이미 검증하던 `ata:`/`ext2:`/`vfs:`/`shell:` 흐름도 그대로 살아있다 — NVMe 컨트롤러 추가가 기존 legacy IDE 경로를 건드리지 않았다.

`e2fsck -f -n build/disk.img`, `grub-file --is-x86-multiboot2 build/kernel.elf` 둘 다 exit 0.

## 이전 단계(65) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/nvme.c`, `boot/nvme.h` | 신규 | NVMe BDF 탐색, 장치 활성화, BAR0 MMIO 매핑, admin queue(SQ/CQ) 초기화, Identify Controller/Namespace 커맨드 제출 + phase bit polling 완료 확인 |
| `boot/pci.c`, `boot/pci.h` | 수정 | `pci_config_write32`/`16`(ECAM 쓰기, 65엔 읽기만 있었음), `pci_find_by_class`(class/subclass로 BDF 검색), `pci_enable_device`(Command 레지스터 Memory Space Enable + Bus Master Enable), `pci_bar_address`(BAR를 물리주소 `u64`로 리턴) 신규 추가 |
| `boot/kernel.c` | 수정 | `#include "nvme.h"` 추가, `pci_scan()`과 `kheap_init()` 사이에 `nvme_init()` 호출 추가 |
| `Makefile` | 수정 | `boot/nvme.c` 빌드 규칙과 `KERNELELF` 링크 목록에 추가, `boot/kernel.c` 빌드 규칙 헤더 의존성에 `boot/nvme.h` 추가, `build/nvme.img`(16MB raw) 타겟 신규, `run`/`run-nogui`에 `-device nvme` 드라이브 추가 |
| 나머지 전부 | 변경 없음 | 65의 파일 그대로 |

## 완료 기준

`make clean && make run-nogui`에서 부팅 배너가 위 "검증" 절의 `nvme:` 블록(BDF 발견 → CAP 파싱 → admin queue ready → Identify Controller 성공(모델명/시리얼/펌웨어) → Identify Namespace 성공(네임스페이스 크기))을 포함해야 하고, `ata:`/`ext2:` 이후 51~65의 나머지 흐름이 정상 동작해야 한다. `e2fsck -f -n build/disk.img`가 에러 없이 통과해야 한다. `grub-file --is-x86-multiboot2 build/kernel.elf`가 exit 0이어야 한다.

## 다음 단계 힌트

- **I/O SQ/CQ 생성과 실제 read/write 커맨드는 구현하지 않았다**: admin 큐 하나로 Identify만 하는 게 이번 단계 스코프다. `67-nvme-io`에서 `Create I/O Completion Queue`/`Create I/O Submission Queue` admin 커맨드로 I/O 큐를 만들고, NVM command set의 Read(`0x02`)/Write(`0x01`) 커맨드로 `51-ata-pio`가 제공하던 섹터 read/write를 대체한다.
- **인터럽트(MSI-X)는 쓰지 않았다**: `00:04.0`의 capability list에 MSI-X가 이미 잡혀있는 걸 봤지만(`cap 0x11: MSI-X`), 이번 단계는 전부 phase bit polling이다. `68-msi-x`에서 이 컨트롤러를 그대로 재사용해 polling 대신 인터럽트로 완료 통지를 받는지만 검증한다.
- **BAR 크기 프로브(0xFFFFFFFF 쓰고 읽어서 실제 크기 역산)는 구현하지 않았다**: `65`가 미뤄뒀던 항목인데, 이번 단계에서 필요한 레지스터(CAP~ACQ)와 admin 큐 doorbell(offset `0x1000`~)이 전부 BAR0의 처음 두 페이지(0~0x1FFF) 안에 고정 오프셋으로 들어오는 게 스펙으로 보장돼 있어서, 두 페이지를 그냥 고정으로 매핑하는 것으로 충분했다. 크기를 몰라도 되는 상황이라 프로브를 추가하지 않았다 — 나중에 BAR가 가리키는 영역 전체(예: 여러 I/O 큐의 doorbell들)를 동적으로 다뤄야 하는 시점이 오면 그때 추가한다.
- **CQ/SQ 모두 64개 엔트리 고정이다**: MQES가 더 작은 하드웨어를 만나면 clamp하도록 짜뒀지만, 더 큰 큐가 필요해지는 시점(예: 여러 I/O 요청을 동시에 in-flight로 돌리는 성능 실습)이 오면 그때 늘린다.
- **Identify Namespace는 NSID=1 하나만 조회한다**: 이 QEMU nvme 장치는 네임스페이스를 하나만 노출하므로 충분했다 — 멀티 네임스페이스 열거(`Identify Namespace List`, `CNS=0x02`)는 필요해지는 시점까지 미룬다.
