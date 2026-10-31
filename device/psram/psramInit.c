/**
 * @file psramInit.c
 *
 * PSRAM device init.  The PSRAM is initialized and mapped by the SBL
 * (mmu_init maps it at 0x48000000), so this device only verifies it and
 * builds the free list used by psram_malloc()/psram_calloc()/psram_free().
 */
/* Embedded Xinu, Copyright (C) 2009.  All rights reserved. */

#include <kernel.h>
#include <device.h>
#include <memory.h>
#include <psram.h>

/**
 * @ingroup psram
 *
 * Initialize the PSRAM free list.
 *
 * @param devptr PSRAM device table entry
 * @return OK
 */

/* PSRAM base address (mapped by MMU) */
#define PSRAM_BASE          0x48000000
#define PSRAM_SIZE          0x02000000  /* 32 MB */

/* PSRAM answers at two addresses (TRM 7.3.3.1): 0x48...... goes through the
 * L1/L2 caches, 0x88...... (+this offset) is the CPU's direct path to the
 * same cells.  The probe below uses the direct path on purpose: walking
 * 32 MB through the cached path evicts the cache lines that hold the
 * kernel's own L2MEM .data, and this chip loses the first 128 bytes of
 * L2MEM (one cache line) on that eviction -- at boot that silently zeroed
 * fs_cwd[] and every shell pwd/cd that followed.  The direct path also
 * makes the test check the PSRAM cells themselves rather than the cache. */
#define PSRAM_CPU_UNCACHED  0x40000000UL


bool psram_test(void)
{
    volatile uint32_t *mem =
        (volatile uint32_t *)(PSRAM_BASE + PSRAM_CPU_UNCACHED);
    static const uint32_t offsets[] = {
        0,
        0x1000,
        0x100000,
        PSRAM_SIZE / 2,
        PSRAM_SIZE - 4,
    };
    bool ok = TRUE;
    uint32_t i;

    for (i = 0; i < (sizeof(offsets) / sizeof(offsets[0])); i++) {
        uint32_t idx = offsets[i] / 4;
        uint32_t saved = mem[idx];
        uint32_t pattern = 0xA5A5A5A5U ^ (i * 0x01010101U);

        mem[idx] = pattern;
        if (mem[idx] != pattern) {
            ok = FALSE;
        }
        mem[idx] = saved;
    }

    return ok;
}

uint32_t psram_get_size(void)
{
    return PSRAM_SIZE;
}



devcall psramInit(device *devptr)
{
    (void)devptr;

    if (psram_test())
    {
        psram_meminit();
        kprintf("[psram] test: OK, %d KiB free\n",
                (int)(psram_free_bytes() >> 10));
    }
    else
    {
        kprintf("[psram] test: FAILED (psram_malloc = NULL)\n");
    }

    return OK;
}
