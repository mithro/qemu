/*
 * ASPEED AST2050 (G3) Video Engine (KVM screen capture).
 *
 * Datasheet §: VR000 protection key (unlock 0x1A038AA8), VR004 capture/compress
 * trigger+status, VR008 source select, VR040-058 DRAM capture buffer bases, INT#7.
 * OpenBMC `aspeed-video` captures the host VGA/CRT framebuffer for KVM. See
 * qemu-model/peripherals/video. Mainline QEMU leaves this a stub.
 *
 * This code is licensed under the GPL version 2 or later.
 */
#ifndef ASPEED_VIDEO_AST2050_H
#define ASPEED_VIDEO_AST2050_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_ASPEED_VIDEO_AST2050 "aspeed.video-ast2050"
OBJECT_DECLARE_SIMPLE_TYPE(AspeedVideoAST2050State, ASPEED_VIDEO_AST2050)

#define ASPEED_VIDEO_AST2050_NR_REGS (0x1000 / 4)

struct AspeedVideoAST2050State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[ASPEED_VIDEO_AST2050_NR_REGS];
};

#endif /* ASPEED_VIDEO_AST2050_H */
