/*
 * ASPEED AST2050 (G3) USB2.0 device / virtual-hub controller.
 *
 * Base 0x1E6A0000 (datasheet): HUB00 root control + device blocks + a 21-endpoint
 * pool with DMA descriptors. This is the BMC virtual-media / virtual-HID path
 * (OpenBMC obmc-ikvm). The AST2050 has NO EHCI host controller — all USB goes
 * through this device/vhub. See qemu-model/peripherals/usb.
 *
 * This code is licensed under the GPL version 2 or later.
 */
#ifndef ASPEED_UDC_AST2050_H
#define ASPEED_UDC_AST2050_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_ASPEED_UDC_AST2050 "aspeed.udc-ast2050"
OBJECT_DECLARE_SIMPLE_TYPE(AspeedUDCAST2050State, ASPEED_UDC_AST2050)

/* Device-controller register window (HUB00 root ctrl + device blocks + EP pool). */
#define ASPEED_UDC_AST2050_NR_REGS (0x300 / 4)

struct AspeedUDCAST2050State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[ASPEED_UDC_AST2050_NR_REGS];
};

#endif /* ASPEED_UDC_AST2050_H */
