/*
 * ASPEED AST2050 (G3) Video Engine (KVM screen capture).
 *
 * A register model: VR000 is a protection-key lock latch (write 0x1A038AA8 to
 * unlock; reads 1 unlocked / 0 locked), the rest are RW while unlocked. This lets
 * the OpenBMC aspeed-video driver bind and program the capture engine. The actual
 * frame-capture/compression from the reserved VGA memory is a deferred behavioural
 * add-on; the register interface here is faithful. See qemu-model/peripherals/video.
 *
 * This code is licensed under the GPL version 2 or later.
 */
#include "qemu/osdep.h"
#include "hw/misc/aspeed_video_ast2050.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define VR_PROTECT   0x000
#define VIDEO_UNLOCK 0x1A038AA8u

static uint64_t aspeed_video_ast2050_read(void *opaque, hwaddr offset, unsigned size)
{
    AspeedVideoAST2050State *s = ASPEED_VIDEO_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_VIDEO_AST2050_NR_REGS) {
        return 0;
    }
    return s->regs[reg];
}

static void aspeed_video_ast2050_write(void *opaque, hwaddr offset, uint64_t data,
                                       unsigned size)
{
    AspeedVideoAST2050State *s = ASPEED_VIDEO_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_VIDEO_AST2050_NR_REGS) {
        return;
    }
    if (offset == VR_PROTECT) {
        /* lock latch: reads back 1 when unlocked, 0 otherwise */
        s->regs[reg] = (data == VIDEO_UNLOCK) ? 1 : 0;
        return;
    }
    if (!s->regs[VR_PROTECT >> 2]) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write while locked at 0x%" HWADDR_PRIx
                      "\n", __func__, offset);
        return;
    }
    s->regs[reg] = data;
}

static const MemoryRegionOps aspeed_video_ast2050_ops = {
    .read = aspeed_video_ast2050_read,
    .write = aspeed_video_ast2050_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void aspeed_video_ast2050_reset(DeviceState *dev)
{
    AspeedVideoAST2050State *s = ASPEED_VIDEO_AST2050(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void aspeed_video_ast2050_realize(DeviceState *dev, Error **errp)
{
    AspeedVideoAST2050State *s = ASPEED_VIDEO_AST2050(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &aspeed_video_ast2050_ops, s,
                          TYPE_ASPEED_VIDEO_AST2050, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription vmstate_aspeed_video_ast2050 = {
    .name = TYPE_ASPEED_VIDEO_AST2050,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, AspeedVideoAST2050State,
                             ASPEED_VIDEO_AST2050_NR_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static void aspeed_video_ast2050_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = aspeed_video_ast2050_realize;
    device_class_set_legacy_reset(dc, aspeed_video_ast2050_reset);
    dc->desc = "ASPEED AST2050 Video Engine";
    dc->vmsd = &vmstate_aspeed_video_ast2050;
}

static const TypeInfo aspeed_video_ast2050_info = {
    .name = TYPE_ASPEED_VIDEO_AST2050,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AspeedVideoAST2050State),
    .class_init = aspeed_video_ast2050_class_init,
};

static void aspeed_video_ast2050_register_types(void)
{
    type_register_static(&aspeed_video_ast2050_info);
}

type_init(aspeed_video_ast2050_register_types)
