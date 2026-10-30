#ifndef MMU_H
#define MMU_H

#include <stdint.h>
#include <stdbool.h>

/* MMU page size (64 KB) */
#define MMU_PAGE_SIZE       0x10000

/* Memory regions */
#define MMU_FLASH_BASE      0x40000000
#define MMU_FLASH_SIZE      0x04000000  /* 64 MB */
#define MMU_PSRAM_BASE      0x48000000
#define MMU_PSRAM_SIZE      0x04000000  /* 64 MB */

/* Initialize MMU: map flash and PSRAM into virtual address space */
bool mmu_init(void);

/* Map a physical page to a virtual address */
bool mmu_map_page(uint32_t vaddr, uint32_t paddr, uint32_t flags);

/* Unmap a virtual page */
bool mmu_unmap_page(uint32_t vaddr);

/* Get physical address from virtual address */
uint32_t mmu_vaddr_to_paddr(uint32_t vaddr);

#endif /* MMU_H */
