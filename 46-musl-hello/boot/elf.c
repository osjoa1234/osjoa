#include "elf.h"
#include "paging.h"
#include "phys_mem.h"
#include "process.h"

#define EM_X86_64 62U

u64 elf_load(const u8 *data, u32 size)
{
    const Elf64_Ehdr *ehdr;
    u16 i;

    if (size < 64U) return 0U;

    ehdr = (const Elf64_Ehdr *)data;

    if (ehdr->e_ident[0] != 0x7FU || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'   || ehdr->e_ident[3] != 'F') return 0U;

    if (ehdr->e_type != 2U || ehdr->e_machine != EM_X86_64) return 0U;

    for (i = 0U; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(
            data + ehdr->e_phoff + (u64)i * (u64)ehdr->e_phentsize);
        u8 *dst;
        u64 j;

        if (phdr->p_type != 1U) continue;

        dst = (u8 *)phdr->p_vaddr;

        for (j = 0U; j < phdr->p_filesz; j++)
            dst[j] = data[phdr->p_offset + j];

        for (j = phdr->p_filesz; j < phdr->p_memsz; j++)
            dst[j] = 0U;
    }

    return ehdr->e_entry;
}

u64 elf_load_process(const u8 *data, u32 size, u32 pml4_phys, u64 *out_brk_start,
                      u64 *out_phdr, u32 *out_phnum, u32 *out_phentsize)
{
    const Elf64_Ehdr *ehdr;
    u16 i;
    u64 brk_end   = 0U;
    u64 phdr_addr = 0U;

    if (size < 64U) return 0U;

    ehdr = (const Elf64_Ehdr *)data;

    if (ehdr->e_ident[0] != 0x7FU || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'   || ehdr->e_ident[3] != 'F') return 0U;

    if (ehdr->e_type != 2U || ehdr->e_machine != EM_X86_64) return 0U;

    for (i = 0U; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(
            data + ehdr->e_phoff + (u64)i * (u64)ehdr->e_phentsize);
        u64 vaddr, page_off, pg;

        if (phdr->p_type != 1U) continue;

        if (phdr->p_vaddr + phdr->p_memsz > brk_end)
            brk_end = phdr->p_vaddr + phdr->p_memsz;

        if (phdr->p_offset <= ehdr->e_phoff &&
            ehdr->e_phoff < phdr->p_offset + phdr->p_filesz)
            phdr_addr = phdr->p_vaddr + (ehdr->e_phoff - phdr->p_offset);

        vaddr    = phdr->p_vaddr & ~0xFFFULL;
        page_off = phdr->p_vaddr - vaddr;

        for (pg = 0U; pg < page_off + phdr->p_memsz; pg += 0x1000U) {
            u32 frame = page_alloc();
            u8 *fdst  = (u8 *)((u64)frame + KERNEL_OFFSET);
            u64 k;

            for (k = 0U; k < 0x1000U; k++) {
                u64 seg_off  = pg + k;
                u64 file_off = seg_off - page_off;

                if (seg_off >= page_off && file_off < phdr->p_filesz)
                    fdst[k] = data[phdr->p_offset + file_off];
                else
                    fdst[k] = 0U;
            }

            paging_map_user_page(pml4_phys, vaddr + pg, frame);
        }
    }

    if (out_brk_start) *out_brk_start = (brk_end + 0xFFFULL) & ~0xFFFULL;
    if (out_phdr)      *out_phdr      = phdr_addr;
    if (out_phnum)     *out_phnum     = (u32)ehdr->e_phnum;
    if (out_phentsize) *out_phentsize = (u32)ehdr->e_phentsize;

    return ehdr->e_entry;
}

#define AT_NULL   0ULL
#define AT_PHDR   3ULL
#define AT_PHENT  4ULL
#define AT_PHNUM  5ULL
#define AT_PAGESZ 6ULL
#define AT_ENTRY  9ULL
#define AT_RANDOM 25ULL
#define AT_SECURE 23ULL

#define USTACK_AUX_PAIRS 7U
#define USTACK_ARGV_MAX  8U

u64 elf_setup_stack(u32 pml4_phys, char *const argv[], u32 argc, u64 entry,
                     u64 phdr, u32 phnum, u32 phentsize)
{
    u64  top      = PROC_USTACK_TOP;
    u64  va;
    u8  *top_page = 0;
    u64  sp_off;
    u64  argv_off[USTACK_ARGV_MAX];
    u64  random_off;
    u64 *frame_words;
    u32  nwords;
    u32  i;
    u32  k;

    if (argc > USTACK_ARGV_MAX) argc = USTACK_ARGV_MAX;

    for (va = top - (u64)PROC_USTACK_PAGES * 0x1000ULL; va < top; va += 0x1000ULL) {
        u32 frame = page_alloc();
        u8 *fdst  = (u8 *)((u64)frame + KERNEL_OFFSET);

        for (k = 0U; k < 0x1000U; k++) fdst[k] = 0U;
        paging_map_user_page(pml4_phys, va, frame);

        if (va == top - 0x1000ULL) top_page = fdst;
    }

    sp_off = 0x1000ULL;

    sp_off -= 16ULL;
    random_off = sp_off;
    for (k = 0U; k < 16U; k++) top_page[random_off + k] = (u8)(0x5AU + k);

    for (i = 0U; i < argc; i++) {
        u32 len = 0U;
        while (argv[i][len]) len++;
        len++;

        sp_off -= (u64)len;
        argv_off[i] = sp_off;
        for (k = 0U; k < len; k++) top_page[sp_off + k] = (u8)argv[i][k];
    }

    nwords  = 1U;
    nwords += argc;
    nwords += 2U;
    nwords += (USTACK_AUX_PAIRS + 1U) * 2U;

    sp_off &= ~0xFULL;
    sp_off -= (u64)nwords * 8ULL;

    frame_words = (u64 *)(top_page + sp_off);
    i = 0U;
    frame_words[i++] = (u64)argc;
    for (k = 0U; k < argc; k++) frame_words[i++] = top - 0x1000ULL + argv_off[k];
    frame_words[i++] = 0ULL;
    frame_words[i++] = 0ULL;
    frame_words[i++] = AT_PHDR;   frame_words[i++] = phdr;
    frame_words[i++] = AT_PHENT;  frame_words[i++] = (u64)phentsize;
    frame_words[i++] = AT_PHNUM;  frame_words[i++] = (u64)phnum;
    frame_words[i++] = AT_PAGESZ; frame_words[i++] = 0x1000ULL;
    frame_words[i++] = AT_ENTRY;  frame_words[i++] = entry;
    frame_words[i++] = AT_RANDOM; frame_words[i++] = top - 0x1000ULL + random_off;
    frame_words[i++] = AT_SECURE; frame_words[i++] = 0ULL;
    frame_words[i++] = AT_NULL;   frame_words[i++] = 0ULL;

    return top - 0x1000ULL + sp_off;
}
