/*
 * ASPEED AST2050 (G3) Video Engine (KVM screen capture).
 *
 * Datasheet §20: VR000 protection key (unlock 0x1A038AA8), VR004 capture/compress
 * trigger+status, VR008 source select, VR040-058 DRAM capture buffer bases,
 * VR304/VR308 interrupt enable/status, INT#7. OpenBMC `aspeed-video` captures the
 * host VGA/CRT framebuffer for KVM. See qemu-model/peripherals/video. Mainline
 * QEMU leaves this a stub.
 *
 * The capture datapath is modelled: the engine reads the internal-VGA scanout
 * (the VGA carve-out at the top of BMC DRAM, sized by the SCU70[3:2] strap),
 * JPEG-compresses it and writes the frame to the driver-programmed compressed-
 * stream buffer, then raises capture/compression-complete on VIC INT#7.
 *
 * This code is licensed under the GPL version 2 or later.
 */
#ifndef ASPEED_VIDEO_AST2050_H
#define ASPEED_VIDEO_AST2050_H

#include "hw/sysbus.h"
#include "qemu/timer.h"
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

    /*
     * Capture datapath: pixel data moves over the M-Bus (direct DRAM access,
     * datasheet §20.2 p.232) — modelled as an AddressSpace on the DRAM
     * MemoryRegion. Buffer-base registers hold *bus* addresses (0x4xxxxxxx),
     * so the DRAM base is needed to convert.
     */
    MemoryRegion *dram_mr;
    AddressSpace dram_as;
    uint64_t dram_base;

    /*
     * Size of the VGA/graphics carve-out at the top of DRAM (SCU70[3:2] strap,
     * referenced by MCR04[5:4], datasheet §18.2 p.217 / §17 p.185). The
     * internal-VGA capture source (VR008[2]=0) scans out of this region.
     */
    uint32_t vga_mem_size;

    /* Modelled internal-VGA source presents a signal (false = no sync). */
    bool vga_signal;

    /* A triggered capture+compression completes after a short delay. */
    QEMUTimer frame_timer;
    bool frame_pending;
};

#endif /* ASPEED_VIDEO_AST2050_H */
