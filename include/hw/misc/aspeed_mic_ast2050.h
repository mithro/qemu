/*
 * ASPEED AST2050 (G3) MIC — Memory Integrity Check Engine (MICE).
 *
 * Datasheet §13 (p116-123), base 0x1E640000, IRQ1. A continuous background DRAM
 * scanner: it reads 4 KB pages starting at address 0x0, computes a per-page
 * Fletcher-32 checksum, and stores 4 bytes/page into a DRAM "checksum buffer"
 * (base MIC04), driven by a 2-bit-per-page "control buffer" in DRAM (base MIC00:
 * 00=skip, 01=ECC-read-only, 10=debug-always-write, 11=MIC-mode-check). On a
 * MIC-mode checksum MISMATCH it raises IRQ1 (level-high) and records the page.
 *
 * The exact Fletcher-32 reduction is taken from the Raptor SLT that drives the
 * real silicon (raptor .../ast2050/mictest.c do_chksum(), which byte-compares
 * software against the hardware output), so the modelled checksum is bit-exact.
 *
 * The scanned region + both metadata buffers live in DRAM at 0x0-based
 * addresses (reachable via the AHBC boot-remap low aperture).
 *
 * This code is licensed under the GPL version 2 or later.
 */
#ifndef ASPEED_MIC_AST2050_H
#define ASPEED_MIC_AST2050_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_ASPEED_MIC_AST2050 "aspeed.mic-ast2050"
OBJECT_DECLARE_SIMPLE_TYPE(AspeedMICAST2050State, ASPEED_MIC_AST2050)

/* Registers MIC00..MIC1C (8 regs); window rounded to 0x20. */
#define ASPEED_MIC_AST2050_NR_REGS (0x20 / 4)

struct AspeedMICAST2050State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[ASPEED_MIC_AST2050_NR_REGS];

    /*
     * Re-entrancy guard: a scan issues address_space accesses driven by guest-
     * programmed buffer addresses; if a buffer aliases this device's own MMIO
     * window the access re-enters the scan path. Not migrated (always false
     * outside aspeed_mic_scan()).
     */
    bool in_scan;
};

#endif /* ASPEED_MIC_AST2050_H */
