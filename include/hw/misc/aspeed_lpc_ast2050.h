/*
 * ASPEED AST2050 (G3) LPC host interface — KCS / BT / iLPC2AHB.
 *
 * Register layout at 0x1E789000 (datasheet): HICR0-4 @0x00-0x10, LADR @0x14-0x20,
 * KCS IDR1-3 @0x24-0x2C / ODR1-3 @0x30-0x38 / STR1-3 @0x3C-0x44, BT @0x48-0x68,
 * SERIRQ @0x70-0x7C, iLPC2AHB HICR5-8 @0x80-0x8C, port-80h snoop @0x90-0x94.
 *
 * This is the G3 layout, NOT the AST2400 aspeed_lpc which puts KCS/iBT at the
 * 0x140 offsets. See qemu-model/peripherals/lpc.
 *
 * The KCS channels implement the faithful H8S/2168-style OBF/IBF/C-D handshake
 * (STRn semantics, datasheet p.315-316) with the IBF interrupt to VIC #8; the
 * host (LPC I/O port) side is driven via the host-kcs<N>-{data,cmdsts} QOM
 * properties (see aspeed_lpc_ast2050.c) since this machine has no host CPU.
 *
 * This code is licensed under the GPL version 2 or later.
 */
#ifndef ASPEED_LPC_AST2050_H
#define ASPEED_LPC_AST2050_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_ASPEED_LPC_AST2050 "aspeed.lpc-ast2050"
OBJECT_DECLARE_SIMPLE_TYPE(AspeedLPCAST2050State, ASPEED_LPC_AST2050)

/* Registers 0x00-0x94 (HICR/LADR/KCS/BT/SERIRQ/iLPC2AHB/snoop). */
#define ASPEED_LPC_AST2050_NR_REGS (0xA0 / 4)

/* LADR12L (0x20) host-I/O address low reset = 0x60 (KCS ch#1/#2 base 0x60/0x62). */
#define ASPEED_LPC_AST2050_LADR12L_RESET 0x00000060

struct AspeedLPCAST2050State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[ASPEED_LPC_AST2050_NR_REGS];
};

#endif /* ASPEED_LPC_AST2050_H */
