#include <stdint.h>
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "mm.h"
#include "kprintf.h"

#define PG_DN(x) ((x) & ~(PAGE_SIZE - 1))
#define PG_UP(x) (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

/* Ensure user virtual page `va` is mapped in `cr3` with at least `flags`;
 * allocate+zero a fresh frame if absent (segments can share a boundary page).
 * Returns the frame physical base, or 0 on OOM. */
static uint64_t ensure_page(uint64_t cr3, uint64_t va, uint64_t flags) {
    uint64_t ph = vmm_get_phys(cr3, va);
    if (ph) return PG_DN(ph);
    uint64_t fr = pmm_alloc_frame();
    if (!fr) return 0;
    uint8_t *kv = (uint8_t *)P2V(fr);
    for (int i = 0; i < (int)PAGE_SIZE; i++) kv[i] = 0;
    if (vmm_map(cr3, va, fr, flags) != 0) { pmm_free_frame(fr); return 0; }
    return fr;
}

int elf_load(uint64_t cr3, const uint8_t *image, uint64_t size,
             uint64_t *entry, uint64_t *brk_end) {
    if (size < sizeof(Elf64_Ehdr)) return -1;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)image;

    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') return -1;
    if (eh->e_ident[4] != ELFCLASS64) return -1;
    if (eh->e_machine != EM_X86_64)   return -1;
    if (eh->e_type != ET_EXEC)        return -1;
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size) return -1;

    uint64_t hi = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph =
            (const Elf64_Phdr *)(image + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        if (ph->p_offset + ph->p_filesz > size) return -1;

        uint64_t flags = PTE_PRESENT | PTE_USER;
        if (ph->p_flags & PF_W) flags |= PTE_WRITE;

        uint64_t va_s = PG_DN(ph->p_vaddr);
        uint64_t va_e = PG_UP(ph->p_vaddr + ph->p_memsz);
        for (uint64_t va = va_s; va < va_e; va += PAGE_SIZE)
            if (!ensure_page(cr3, va, flags)) return -1;

        /* copy file-backed bytes through the per-page translation; .bss tail is
         * already zero from ensure_page(). */
        for (uint64_t j = 0; j < ph->p_filesz; j++) {
            uint64_t uva = ph->p_vaddr + j;
            uint64_t dst = vmm_get_phys(cr3, uva);
            if (!dst) return -1;
            *(uint8_t *)P2V(dst) = image[ph->p_offset + j];
        }
        if (va_e > hi) hi = va_e;
    }
    if (!hi) return -1;             /* no loadable segment */

    *entry   = eh->e_entry;
    *brk_end = hi;
    return 0;
}
