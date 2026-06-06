#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ELF64 header and program header definitions.
 * These are the minimal structures needed to load a statically-linked
 * x86_64 executable into a fresh address space. */

#define ELFMAG0         0x7f
#define ELFMAG1         'E'
#define ELFMAG2         'L'
#define ELFMAG3         'F'
#define ELFCLASS64      2
#define ELFDATA2LSB     1
#define EV_CURRENT      1
#define ET_EXEC         2
#define EM_X86_64       62

#define PT_LOAD         1

#define PF_X            1
#define PF_W            2
#define PF_R            4

typedef struct {
    uint8_t  ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} elf64_ehdr_t;

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} elf64_phdr_t;

/* Validate that 'data' points to a loadable x86_64 ELF.
 * Returns true and fills *ehdr if valid. */
bool elf_validate(const uint8_t *data, size_t len, elf64_ehdr_t *ehdr);

/* Load a validated ELF into a fresh address space.
 *
 * data    - pointer to the ELF file in kernel memory
 * len     - size of the ELF file
 * pml4    - root page table to map segments into
 * entry   - filled with the entry-point virtual address
 *
 * Returns true on success.  On failure partial mappings may exist. */
bool elf_load(const uint8_t *data, size_t len, uint64_t *pml4,
              uint64_t *entry);
