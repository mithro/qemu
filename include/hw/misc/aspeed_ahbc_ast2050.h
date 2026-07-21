/*
 * ASPEED AST2050 (G3) AHB Bus Controller (AHBC).
 *
 * Datasheet §12 (p113-115): base 0x1E600000, IRQ31. Four registers:
 *   AHBC00 protection key
 *   AHBC80 priority control
 *   AHBC88 interrupt (bus-error) control
 *   AHBC8C Address Remap — [0] "Boot Area Remap": 0 = 0x0..0x0FFFFFFF maps to
 *          Static memory (flash), 1 = maps to SDRAM (§12.3 p115). Software sets
 *          it to 1 after DRAM init so the low 256 MB aperture is SDRAM — which is
 *          how the 28-bit MDMA engine (§22) reaches DRAM.
 *
 * This model backs the register file and implements the boot-remap: when
 * AHBC8C[0] is written, it enables/disables a caller-provided SDRAM alias mapped
 * at 0x0 (remap_mr). The alias is DEFAULT-OFF (reset = boot from static memory),
 * so the C2/C4/C-UBOOT boot oracles — which never set AHBC8C[0] — are unaffected.
 *
 * This code is licensed under the GPL version 2 or later.
 */
#ifndef ASPEED_AHBC_AST2050_H
#define ASPEED_AHBC_AST2050_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_ASPEED_AHBC_AST2050 "aspeed.ahbc-ast2050"
OBJECT_DECLARE_SIMPLE_TYPE(AspeedAHBCAST2050State, ASPEED_AHBC_AST2050)

/* Registers span 0x00..0x8C; round the window to 0x90. */
#define ASPEED_AHBC_AST2050_NR_REGS (0x90 / 4)

struct AspeedAHBCAST2050State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    /*
     * SDRAM alias mapped at 0x0 by the SoC (default-disabled); AHBC8C[0]
     * enables/disables it. Set by the SoC before realize; may be NULL.
     */
    MemoryRegion *remap_mr;

    uint32_t regs[ASPEED_AHBC_AST2050_NR_REGS];

    /* §12.3: writes to 0x80..0x8C require the 0xAEED1A03 key at 0x00 first. */
    bool unlocked;
};

#endif /* ASPEED_AHBC_AST2050_H */
