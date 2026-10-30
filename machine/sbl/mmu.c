/******************************************************************************************
  Filename    : mmu.c
  
  Core        : RISC-V
  
  MCU         : ESP32-P4
    
  Author      : Chalandi Amine
 
  Owner       : Chalandi Amine
  
  Date        : 25.01.2026
  
  Description : Initialize MMU to map the entire address range of FLASH and PSRAM
  
******************************************************************************************/

#include <stdint.h>

#define MMU_FLASH_TLB_ENTRY_REG            *(volatile uint32_t*)(0x5008c380ul)
#define MMU_FLASH_TLB_VALUE_REG            *(volatile uint32_t*)(0x5008c37cul)
#define MMU_PSRAM_TLB_ENTRY_REG            *(volatile uint32_t*)(0x5008e380ul)
#define MMU_PSRAM_TLB_VALUE_REG            *(volatile uint32_t*)(0x5008e37cul)
#define MMU_TLB_INVALIDATE_VALUE           0x0ul
#define MMU_TLB_PAGE_SIZE                  0x10000ul //64kB
#define MMU_TLB_PAGE_ALIGN                 16
#define MMU_FLASH_TLB_MARK_AS_VALID_ENTRY  (1ul << 12)
#define MMU_PSRAM_TLB_MARK_AS_VALID_ENTRY  (1ul << 11) | (1ul << 10)

void mmu_init(void);

//-----------------------------------------------------------------------------------------
/// \brief  
///
/// \param  
///
/// \return 
//-----------------------------------------------------------------------------------------
void mmu_init(void)
{
    /* invalidate the TLB entries */
    for(uint32_t mmu_tlb_entry = 0; mmu_tlb_entry < 1024; mmu_tlb_entry++)
    {
        MMU_FLASH_TLB_ENTRY_REG = mmu_tlb_entry;
        MMU_FLASH_TLB_VALUE_REG = MMU_TLB_INVALIDATE_VALUE;
    }

    /* write new TLB entries : Map QSPI NOR-Flash physical address 0 to "virtual" address 0x40000000 */
    /* note: the cache is enabled in the bootROM no need to re-initalize it */

    for(uint32_t mmu_tlb_entry = 0; mmu_tlb_entry < 512; mmu_tlb_entry++)
    {
        MMU_FLASH_TLB_ENTRY_REG = mmu_tlb_entry;
        MMU_FLASH_TLB_VALUE_REG = MMU_FLASH_TLB_MARK_AS_VALID_ENTRY | (uint32_t)((MMU_TLB_PAGE_SIZE * mmu_tlb_entry) >> MMU_TLB_PAGE_ALIGN);
    }

    /* note: PSRAM is not initialized by the bootROM : PSRAM initialization should be called before this code */
    for(uint32_t mmu_tlb_entry = 0; mmu_tlb_entry < 1024; mmu_tlb_entry++)
    {
        MMU_PSRAM_TLB_ENTRY_REG = mmu_tlb_entry;
        MMU_PSRAM_TLB_VALUE_REG = MMU_TLB_INVALIDATE_VALUE;
    }

    /* fill the TLB */
    for(uint32_t mmu_tlb_entry = 0; mmu_tlb_entry < 512; mmu_tlb_entry++)
    {
        MMU_PSRAM_TLB_ENTRY_REG = mmu_tlb_entry;
        MMU_PSRAM_TLB_VALUE_REG = MMU_PSRAM_TLB_MARK_AS_VALID_ENTRY | (uint32_t)((MMU_TLB_PAGE_SIZE * mmu_tlb_entry) >> MMU_TLB_PAGE_ALIGN);
    }
}
