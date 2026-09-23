# 63 — multiboot2

**목표**: GRUB 부팅 방식을 Multiboot1에서 Multiboot2로 업그레이드한다. `08-grub-multiboot`가 넘겨받던 고정 크기 `multiboot_info` 구조체를 Multiboot2의 **태그 순회(tag iteration)** 방식으로 교체하고, Multiboot2가 새로 제공하는 **ACPI RSDP 태그**(구버전 14 / 신버전 15)를 받아온다. 이 RSDP는 `64-apic`에서 MADT(IOAPIC/LAPIC 주소 테이블)를 파싱하는 전제조건이다 — Multiboot1에는 ACPI 태그가 없어서 RSDP를 EBDA나 `0xE0000`~`0xFFFFF` 영역에서 수동으로 스캔해야 했는데, 이번 전환으로 그 수동 스캔 자체가 필요 없어진다(그래서 이 프로젝트엔 그 스캔 코드가 존재한 적이 없다).

## 1) 부트 헤더: Multiboot1 3-word 헤더 → Multiboot2 태그 헤더

`boot/entry.asm`의 `.boot` 섹션 맨 앞, GRUB이 커널 ELF를 찾아 읽는 매직 헤더를 교체했다. Multiboot1 헤더는 `magic`/`flags`/`checksum` 세 워드가 전부였지만, Multiboot2 헤더는 자신도 태그 구조를 따른다 — 헤더 자체가 "태그 목록"이고, 최소 구성은 "종료 태그(type=0, size=8)" 하나뿐이다:

```asm
align 8
mb2_header_start:
dd 0xE85250D6                                          ; magic
dd 0x00000000                                           ; architecture: i386
dd mb2_header_end - mb2_header_start                     ; header_length
dd -(0xE85250D6 + 0x00000000 + (mb2_header_end - mb2_header_start))  ; checksum

align 8
dw 0x0000                                                ; end tag: type=0
dw 0x0000                                                ;          flags=0
dd 0x00000008                                            ;          size=8
mb2_header_end:
```

Multiboot1의 `0x1BADB002`/`flags=0x3`(MEMINFO+MODALIGN 비트) 조합은 없어졌다 — Multiboot2에서 메모리맵과 모듈 정보는 항상 boot info 쪽 태그로 오고, "이 플래그를 켜야 그 정보를 넣어준다"는 협상 자체가 사라졌다. 헤더는 8바이트 정렬이어야 한다는 점도 Multiboot1(4바이트)과 다르다.

`start:` 진입 이후 롱모드 전환 코드(PML4/PDPT/PD 구성, `CR4`/`EFER`/`CR0`, GDT64 로드)는 전혀 건드리지 않았다 — EAX(매직)/EBX(boot info 포인터)를 `saved_magic`/`saved_mbi`에 저장해 `kernel_main`에 그대로 넘기는 배관은 부트로더가 뭘 쓰든 동일하기 때문이다.

## 2) `grub/grub.cfg`: `multiboot`/`module` → `multiboot2`/`module2`

```
menuentry "custom-os" {
    multiboot2 /boot/kernel.elf
    module2 /boot/initramfs.cpio
    boot
}
```

GRUB의 `multiboot`/`module` 커맨드는 Multiboot1 프로토콜로 커널을 불러오고, `multiboot2`/`module2`는 Multiboot2 프로토콜로 불러온다 — 커널 쪽 헤더를 MB2로 바꿨으면 grub.cfg도 반드시 같이 바꿔야 한다(안 바꾸면 GRUB이 옛 프로토콜로 EAX/EBX를 채워 넘기고, 커널의 매직 검사에서 바로 걸린다).

`Makefile`의 `grub-file --is-x86-multiboot`도 `--is-x86-multiboot2`로 바꿨다 — 이건 그냥 "커널 ELF가 해당 프로토콜 헤더를 갖고 있는지" 확인하는 빌드 타임 체크라 실제 부팅 동작엔 영향 없다.

## 3) `boot/kernel.c`: 고정 구조체 → 태그 순회

Multiboot1의 `struct multiboot_info`는 필드 오프셋이 고정된 하나의 구조체였고, "이 정보가 있는지"는 `flags` 비트마스크로 확인했다(`flags & (1<<6)`이면 mmap, `flags & (1<<3)`이면 모듈). Multiboot2는 이 고정 구조체를 버리고, boot info 자체가 **가변 길이 태그의 연속**이다:

```
struct multiboot2_info { u32 total_size; u32 reserved; };   // 헤더 8바이트
[tag 1][tag 2] ... [tag N][end tag(type=0, size=8)]
```

각 태그는 `{ u32 type; u32 size; ... }`이고, 다음 태그는 `size`를 8바이트 경계로 올림한 위치에서 시작한다(`size` 자체엔 패딩이 포함 안 됨). "이 정보가 있는가"는 더 이상 플래그 비트가 아니라 "그 타입의 태그를 찾았는가"다:

```c
static const struct multiboot2_tag *multiboot2_find_tag(const struct multiboot2_info *mbi, u32 type)
{
    const u8 *ptr = (const u8 *)mbi + sizeof(struct multiboot2_info);
    const u8 *end = (const u8 *)mbi + mbi->total_size;

    while (ptr < end) {
        const struct multiboot2_tag *tag = (const struct multiboot2_tag *)ptr;
        if (tag->type == MULTIBOOT2_TAG_END) break;
        if (tag->type == type) return tag;
        ptr += (tag->size + 7U) & ~7U;
    }
    return 0;
}
```

매직값도 `0x2BADB002`(Multiboot1) → `0x36D76289`(Multiboot2)로 바뀐다. `kernel_main`은 여전히 `magic`/`phys_mbi`를 EAX/EBX 레지스터 값 그대로 받는다(entry.asm은 안 바뀌었으므로) — `mbi` 포인터에 `KERNEL_OFFSET`을 더해 가상주소로 바꾸는 부분까지 동일하고, 그 뒤로 이 포인터를 어떻게 해석하느냐만 바뀐 것이다.

### mmap: `mmap_addr`/`mmap_length` 필드 → 타입 6 태그, entry_size 순회

Multiboot1의 mmap은 "가변 크기 엔트리"(각 엔트리 앞에 자기 `size` 필드가 있고, 다음 엔트리는 `offset += e->size + 4`로 찾아가는 독특한 인코딩)였다. Multiboot2의 mmap 태그(`type=6`)는 훨씬 단순한 **고정 스트라이드** 배열이다 — 태그 자체에 `entry_size`가 박혀 있고, 모든 엔트리가 그 크기로 나란히 붙어 있다:

```c
struct multiboot2_tag_mmap {
    u32 type, size, entry_size, entry_version;
    struct multiboot2_tag_mmap_entry entries[];   // { u64 base_addr; u64 length; u32 type; u32 reserved; }
};
```

`addr_low`/`addr_high`로 쪼개져 있던 Multiboot1과 달리 `base_addr`/`length`가 처음부터 `u64` 하나로 온다. 이 변화가 `boot/paging.c`의 `paging_init`과 `boot/phys_mem.c`의 `phys_mem_init` 양쪽에 그대로 반영됐다 — 두 함수 다 시그니처에 `entry_size` 파라미터가 추가됐고(`offset += e->size + 4U` → `offset += entry_size`), 내부 `e820_entry`/`mmap_entry` 구조체가 `addr_low/addr_high/len_low/len_high` 5필드에서 `base_addr/length` 2필드(+`type`/`reserved`)로 줄었다:

```c
// paging.c / phys_mem.c 공통 패턴
struct e820_entry { u64 base_addr; u64 length; u32 type; u32 reserved; };

while (offset < mmap_length) {
    const struct e820_entry *e = (const struct e820_entry *)(mmap_vaddr + offset);
    if (e->type == 1U && (e->base_addr >> 32) == 0U) { ... }
    offset += entry_size;
}
```

`kernel_main`에서 호출부는 다음과 같이 바뀌었다:

```c
const struct multiboot2_tag_mmap *mmap_tag =
    (const struct multiboot2_tag_mmap *)multiboot2_find_tag(mbi, MULTIBOOT2_TAG_MMAP);

if (mmap_tag) {
    u64 entries_vaddr  = (u64)mmap_tag->entries;
    u32 entries_length = mmap_tag->size - (u32)sizeof(struct multiboot2_tag_mmap);
    paging_init(entries_vaddr, entries_length, mmap_tag->entry_size);
    phys_mem_init(entries_vaddr, entries_length, mmap_tag->entry_size, kend_phys);
}
```

`mmap_tag`가 이미 `mbi`(가상주소)를 기준으로 한 오프셷 포인터이므로 `entries_vaddr`에 `KERNEL_OFFSET`을 또 더할 필요가 없다 — Multiboot1에서 `mbi->mmap_addr`가 **물리주소**라 매번 `+ KERNEL_OFFSET`을 붙였던 것과 다른 지점이다(boot info 구조체 자신은 GRUB이 물리주소로 내려주지만, 그 안에 임베드된 태그 데이터는 구조체와 같은 메모리 블록에 이어붙어 있어서 구조체 시작 주소만 가상화하면 내부 포인터 연산은 자동으로 맞아떨어진다). 반면 모듈의 `mod_start`/`mod_end`는 태그 밖의 **별도 메모리 영역**(initramfs 페이로드 자체)을 가리키는 물리주소라서 여전히 역참조할 때 `+ KERNEL_OFFSET`이 필요하다(아래 4번 참고).

### 모듈: `mods_addr` 배열 → 타입 3 태그(모듈당 하나씩 반복)

Multiboot1은 모듈이 여러 개면 `mods_addr`가 가리키는 위치에 `multiboot_mod` 배열이 연속으로 있었다. Multiboot2는 모듈마다 별도의 `type=3` 태그가 하나씩 있다 — 지금은 initramfs 하나뿐이라 `multiboot2_find_tag(mbi, MULTIBOOT2_TAG_MODULE)`로 첫 번째 것만 찾으면 기존 `mod0[0]` 접근과 동일하다. `phys_mem_reserve`(모듈이 차지한 물리 페이지를 프리 리스트에서 빼는 호출)와 `initrd_init` 두 곳에서 각각 한 번씩 태그를 다시 찾는 것도 기존 코드가 `mods_addr`를 두 번 역참조하던 구조 그대로다.

### ACPI RSDP: 이번 단계에서 새로 추가된 것

```c
const struct multiboot2_tag_rsdp *rsdp_tag =
    (const struct multiboot2_tag_rsdp *)multiboot2_find_tag(mbi, MULTIBOOT2_TAG_ACPI_NEW);
if (!rsdp_tag) {
    rsdp_tag = (const struct multiboot2_tag_rsdp *)multiboot2_find_tag(mbi, MULTIBOOT2_TAG_ACPI_OLD);
}
```

타입 15(ACPI 2.0+, XSDT를 가리키는 확장 RSDP)를 먼저 찾고 없으면 타입 14(ACPI 1.0, RSDT만 가리키는 구버전 RSDP)로 폴백한다 — 실제 리눅스 커널의 ACPI 부트업 코드(`acpi_os_get_root_pointer` 계열)도 신버전을 우선하고 구버전으로 떨어지는 순서를 따른다. `rsdp_tag->rsdp[]`가 GRUB이 복사해둔 RSDP 구조체 원본 바이트인데, 이번 단계는 "받았다"만 검증하고 `Signature`/`Checksum`/`RsdtAddress` 같은 내부 필드는 파싱하지 않는다(그건 MADT를 실제로 찾아가야 하는 `64-apic`의 일) — QEMU 기본 SeaBIOS 환경에서는 타입 14(ACPI 1.0, 20바이트)만 온다:

```
acpi: RSDP tag received (ACPI 1.0, 20 bytes) at 0xFFFFFFFF80100560
```

## 검증

`make clean && make run-nogui` 부팅 배너가 다음과 같이 바뀌었다(관련 구간만):

```
Hello world -- long mode (64-bit), processes, multiboot2
GDT ready: null/kcode64/kdata/ucode64/udata/TSS(0x28)
syscall: SYSCALL/SYSRET MSR entry ready (LSTAR=0x...)
paging: 4-level PAE active, 2MB huge pages, 1GB identity+kernel
phys mem: 32557 free pages (127MB usable)
acpi: RSDP tag received (ACPI 1.0, 20 bytes) at 0xFFFFFFFF80100560
IDT ready: 256 entries (16-byte gates) PIC=0x20/0x28
heap: kernel dir adopted, window at 0x... (mapped=128MB)
...
initramfs: 13 file(s) found
vfs: initrd mounted at /
vfs: ext2 mounted at /disk/
```

`grub-file --is-x86-multiboot2 build/kernel.elf`가 성공(exit 0)한다 — 커널 ELF가 유효한 Multiboot2 헤더를 갖고 있다는 뜻이다.

**회귀**: 62까지의 전체 부팅 로그를 `diff`로 비교한 결과, 배너 문구(`multiboot2` 표기)와 `acpi:` 신규 라인, 그리고 코드 크기 변화로 인한 `LSTAR` 주소값 한 곳을 제외하면 **완전히 동일**하다 — `phys mem: 32557 free pages (127MB usable)`(mmap 파싱 결과), `initramfs: 13 file(s) found`(모듈 파싱 결과), 이후 51~62의 ext2/getdents/심링크/PATH exec/파이프/리다이렉션/`fcntl` 검증 전부가 62와 한 글자도 다르지 않게 통과했다. `e2fsck -f -n build/disk.img`도 에러 없이 통과했다.

## 완료 기준

`make clean && make run-nogui`에서 부팅 배너가 위 "검증" 절의 `acpi:` 라인을 포함해야 하고, 그 이후 51~62의 모든 출력이 62와 동일해야 한다. `grub-file --is-x86-multiboot2 build/kernel.elf`가 exit 0이어야 한다. `e2fsck -f -n build/disk.img`가 에러 없이 통과해야 한다.

## 이전 단계(62) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/entry.asm` | 수정 | Multiboot1 3-word 헤더(`0x1BADB002`)를 Multiboot2 태그 헤더(`0xE85250D6` + 종료 태그)로 교체. 롱모드 전환 코드는 무변경 |
| `grub/grub.cfg` | 수정 | `multiboot`/`module` → `multiboot2`/`module2` |
| `Makefile` | 수정 | `grub-file --is-x86-multiboot` → `--is-x86-multiboot2` |
| `boot/kernel.c` | 수정 | `struct multiboot_info`(고정 구조체+flags 비트마스크)를 `struct multiboot2_info`+태그 순회(`multiboot2_find_tag`)로 교체; 매직값 `0x2BADB002`→`0x36D76289`; mmap/모듈 파싱을 태그 기반으로 재작성; ACPI RSDP 태그(14/15) 조회 + 로그 출력 신규 추가 |
| `boot/paging.c`, `boot/paging.h` | 수정 | `e820_entry`를 `addr_low/addr_high/len_low/len_high` 5필드에서 Multiboot2 mmap 엔트리와 같은 `base_addr/length` 2필드(+`type`/`reserved`)로 교체; `paging_init`에 `entry_size` 파라미터 추가, 순회를 `e->size+4` 트릭 대신 고정 스트라이드로 |
| `boot/phys_mem.c`, `boot/phys_mem.h` | 수정 | `paging.c`와 동일한 변경을 `mmap_entry`/`phys_mem_init`에 적용 |
| 나머지 전부 | 변경 없음 | 62의 파일 그대로 |

## 다음 단계 힌트

- **`64-apic`가 RSDP를 실제로 파싱해야 한다**: 이번 단계는 RSDP 태그를 "찾아서 주소를 로그로 찍는" 데서 멈췄다. 다음 단계는 이 포인터(또는 `kernel_main`에서 다시 태그를 찾는 동일한 경로)로 RSDT/XSDT를 따라가 MADT(Multiple APIC Description Table)를 찾고, 거기서 IOAPIC/LAPIC 베이스 주소를 뽑아야 한다. 지금 `rsdp_tag`는 `kernel_main`의 지역 변수라 그대로 사라진다 — 64에서 전역으로 뺄지, 태그를 다시 찾을지는 그때 판단.
- **Multiboot2의 다른 태그들(부트 커맨드라인, 부트로더 이름, 프레임버퍼, ELF 심볼 등)은 여전히 미파싱**: 지금 당장 필요한 게 mmap/모듈/RSDP 셋뿐이라 다른 태그 타입은 건드리지 않았다. 필요해지면 `multiboot2_find_tag`를 그대로 재사용하면 된다.
- **`65-pcie-enum`으로 이동**: RSDP까지 확보했으니 로드맵상 다음은 `64-apic`(IOAPIC/LAPIC, x2APIC 기본)이고, 그 다음이 PCIe 버스 스캔이다.
