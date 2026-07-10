/*
 * ASPEED AST2050 (G3) legacy Static Memory Controller (SMC).
 *
 * Control registers at 0x16000000 (datasheet §11) — the *legacy* SMC, NOT the
 * AST2400 FMC at 0x1E620000 that mainline QEMU's aspeed_smc models. The flash
 * data windows (0x10000000 CE0 / 0x12000000 CE1 / 0x14000000 CE2) are a separate
 * AHB mapping and are not modelled here. See qemu-model/peripherals/smc.
 *
 * This code is licensed under the GPL version 2 or later.
 */
#ifndef ASPEED_SMC_AST2050_H
#define ASPEED_SMC_AST2050_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_ASPEED_SMC_AST2050 "aspeed.smc-ast2050"
OBJECT_DECLARE_SIMPLE_TYPE(AspeedSMCAST2050State, ASPEED_SMC_AST2050)

/* Control registers 0x00-0x1C (config, CE0-CE2 control, misc / NAND ECC). */
#define ASPEED_SMC_AST2050_NR_REGS (0x20 / 4)

/* SMC00 config reset 0x00000240: CE0=NOR, CE1=NAND, CE2=SPI, 32 MB segments. */
#define ASPEED_SMC_AST2050_CONFIG_RESET 0x00000240

struct AspeedSMCAST2050State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    uint32_t regs[ASPEED_SMC_AST2050_NR_REGS];
};

#endif /* ASPEED_SMC_AST2050_H */
