#include "elf.h"
#include "paging.h"
#include "phys_mem.h"

#define PROC_USTACK_TOP 0x00400000U

u32 elf_load(const u8 *data, u32 size)
{
    const Elf32_Ehdr *ehdr;
    u16 i;

    if (size < 52U) return 0U;

    ehdr = (const Elf32_Ehdr *)data;

    if (ehdr->e_ident[0] != 0x7FU || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'   || ehdr->e_ident[3] != 'F') return 0U;

    if (ehdr->e_type != 2U || ehdr->e_machine != 3U) return 0U;

    for (i = 0U; i < ehdr->e_phnum; i++) {
        const Elf32_Phdr *phdr = (const Elf32_Phdr *)(
            data + ehdr->e_phoff + (u32)i * (u32)ehdr->e_phentsize);
        u8 *dst;
        u32 j;

        if (phdr->p_type != 1U) continue;

        dst = (u8 *)phdr->p_vaddr;

        for (j = 0U; j < phdr->p_filesz; j++)
            dst[j] = data[phdr->p_offset + j];

        for (j = phdr->p_filesz; j < phdr->p_memsz; j++)
            dst[j] = 0U;
    }

    return ehdr->e_entry;
}

u32 elf_load_process(const u8 *data, u32 size, u32 pd_phys)
{
    const Elf32_Ehdr *ehdr;
    u16 i;

    if (size < 52U) return 0U;

    ehdr = (const Elf32_Ehdr *)data;

    if (ehdr->e_ident[0] != 0x7FU || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'   || ehdr->e_ident[3] != 'F') return 0U;

    if (ehdr->e_type != 2U || ehdr->e_machine != 3U) return 0U;

    for (i = 0U; i < ehdr->e_phnum; i++) {
        const Elf32_Phdr *phdr = (const Elf32_Phdr *)(
            data + ehdr->e_phoff + (u32)i * (u32)ehdr->e_phentsize);
        u32 vaddr, pg;

        if (phdr->p_type != 1U) continue;

        vaddr = phdr->p_vaddr & ~0xFFFU;

        for (pg = 0U; pg < phdr->p_memsz; pg += 0x1000U) {
            u32 frame  = page_alloc();
            u8 *fdst   = (u8 *)frame;
            u32 foff   = pg;
            u32 k;

            for (k = 0U; k < 0x1000U; k++) fdst[k] = 0U;

            if (foff < phdr->p_filesz) {
                u32 copy = phdr->p_filesz - foff;
                if (copy > 0x1000U) copy = 0x1000U;
                for (k = 0U; k < copy; k++)
                    fdst[k] = data[phdr->p_offset + foff + k];
            }

            paging_map_user_page(pd_phys, vaddr + pg, frame);
        }
    }

    {
        u32 stack_frame = page_alloc();
        u8 *s = (u8 *)stack_frame;
        u32 k;
        for (k = 0U; k < 0x1000U; k++) s[k] = 0U;
        paging_map_user_page(pd_phys, PROC_USTACK_TOP - 0x1000U, stack_frame);
    }

    return ehdr->e_entry;
}
