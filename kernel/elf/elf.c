#include "kernel/elf/elf.h"
#include "kernel/memory/vmm.h"
#include "kernel/memory/pmm.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

bool elf_validate(const uint8_t *data, size_t len, elf64_ehdr_t *ehdr) {
    if (len < sizeof(elf64_ehdr_t)) return false;

    memcpy(ehdr, data, sizeof(elf64_ehdr_t));

    if (ehdr->ident[0] != ELFMAG0 || ehdr->ident[1] != ELFMAG1 ||
        ehdr->ident[2] != ELFMAG2 || ehdr->ident[3] != ELFMAG3)
        return false;
    if (ehdr->ident[4] != ELFCLASS64)  return false;
    if (ehdr->ident[5] != ELFDATA2LSB) return false;
    if (ehdr->ident[6] != EV_CURRENT)  return false;
    if (ehdr->type != ET_EXEC)         return false;
    if (ehdr->machine != EM_X86_64)    return false;
    if (ehdr->phentsize < sizeof(elf64_phdr_t)) return false;
    if (ehdr->phoff + (uint64_t)ehdr->phnum * ehdr->phentsize > len)
        return false;

    return true;
}

bool elf_load(const uint8_t *data, size_t len, uint64_t *pml4,
              uint64_t *entry) {
    elf64_ehdr_t ehdr;
    if (!elf_validate(data, len, &ehdr)) return false;

    *entry = ehdr.entry;

    for (uint16_t i = 0; i < ehdr.phnum; i++) {
        const uint8_t *phdr_raw = data + ehdr.phoff + (uint64_t)i * ehdr.phentsize;
        const elf64_phdr_t *ph = (const elf64_phdr_t *)phdr_raw;

        if (ph->type != PT_LOAD) continue;

        uint64_t vaddr = ph->vaddr;
        uint64_t filesz = ph->filesz;
        uint64_t memsz  = ph->memsz;
        uint64_t offset = ph->offset;

        if (offset + filesz > len) return false;

        uint64_t flags = 0;
        if (ph->flags & PF_R) flags |= VMM_U;
        if (ph->flags & PF_W) flags |= VMM_U | VMM_RW;
        if (ph->flags & PF_X) flags |= VMM_U | VMM_X;

        /* Map each page of the segment. */
        uint64_t seg_end = vaddr + memsz;
        for (uint64_t va = vaddr & ~0xFFFULL; va < seg_end; va += 0x1000) {
            uint64_t page = pmm_alloc_frame();
            if (!page) {
                kprintf("elf_load: out of physical memory at va=%p\n", (void *)va);
                return false;
            }
            void *vpage = phys_to_virt(page);
            memset(vpage, 0, 0x1000);

            uint64_t src_off = offset + (va - vaddr);

            if (src_off < offset + filesz) {
                uint64_t to_copy = 0x1000;
                if (src_off + to_copy > offset + filesz)
                    to_copy = (offset + filesz) - src_off;
                memcpy(vpage, data + src_off, to_copy);
            }

            vmm_map_page(pml4, va, page, flags);
        }
    }

    return true;
}
