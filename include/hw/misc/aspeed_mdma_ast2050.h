/*
 * ASPEED AST2050 (G3) MDMA — memory-copy / memory-fill DMA engine.
 *
 * Datasheet §22 (p257-261): base 0x1E740000, IRQ6 ("MDMA interrupt", §10 Table
 * 36). Six 32-bit registers:
 *   MDMA00 source address  [27:0]
 *   MDMA04 dest address     [27:0]
 *   MDMA08 buffer-fill data [31:0]
 *   MDMA0C command  — the WRITE fires the command (no start bit):
 *          [31] update-status-on-done, [30:28] command ID #0-7,
 *          [25:24] type (00=copy, 10=fill), [23:0] length in BYTES (max 16M-1)
 *   MDMA10 IRQ control (Init 0): [23:16] per-ID mask, [3] irq-when-idle,
 *          [1] irq-on-overflow
 *   MDMA14 IRQ status (Init 0x100): [23:16] per-ID done (W1C), [8:4] RO queue
 *          length (reset 16), [3] idle (W1C), [1] overflow (W1C), [0] RO busy
 *
 * Addresses are 28-bit (256 MB). On silicon the low 256 MB is SDRAM once the
 * AHBC boot-remap (AHBC8C[0]) is on; this model performs the transfer against
 * the system address space with the raw 28-bit address (faithful AHB decode).
 *
 * This code is licensed under the GPL version 2 or later.
 */
#ifndef ASPEED_MDMA_AST2050_H
#define ASPEED_MDMA_AST2050_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_ASPEED_MDMA_AST2050 "aspeed.mdma-ast2050"
OBJECT_DECLARE_SIMPLE_TYPE(AspeedMDMAAST2050State, ASPEED_MDMA_AST2050)

/* Register file is MDMA00..MDMA14 (6 regs); round the window to 0x20. */
#define ASPEED_MDMA_AST2050_NR_REGS (0x20 / 4)

struct AspeedMDMAAST2050State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[ASPEED_MDMA_AST2050_NR_REGS];

    /*
     * Re-entrancy guard: a command executes a DMA to a guest-controlled dst; if
     * dst aliases this device's own MMIO window the write re-enters the command
     * path. Not migrated (always false outside aspeed_mdma_do_command()).
     */
    bool in_command;
};

#endif /* ASPEED_MDMA_AST2050_H */
