#include "elf.h"

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
