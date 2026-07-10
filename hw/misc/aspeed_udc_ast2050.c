/*
 * ASPEED AST2050 (G3) USB2.0 device / virtual-hub controller.
 *
 * Datasheet: base 0x1E6A0000, HUB00 root control at 0x00, device blocks and a
 * 21-endpoint pool with DMA descriptors. This is the BMC's virtual-media /
 * virtual-HID datapath (OpenBMC obmc-ikvm). The AST2050 has NO EHCI host
 * (that's an AST2400+ block) — all USB is via this device/vhub.
 *
 * Register-accurate model: the device-controller register file is RW; the full
 * USB device semantics (enumeration, endpoint DMA, the actual media transport)
 * are refinements. The vendor firmware pokes HUB00 at init; OpenBMC's aspeed-udc
 * driver binds here. Sized to the register file so 0x1E6A1000 stays unmapped
 * (the AST2050 has no EHCI there).
 *
 * This code is licensed under the GPL version 2 or later.
 */
#include "qemu/osdep.h"
#include "hw/misc/aspeed_udc_ast2050.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

static uint64_t aspeed_udc_ast2050_read(void *opaque, hwaddr offset, unsigned size)
{
    AspeedUDCAST2050State *s = ASPEED_UDC_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_UDC_AST2050_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds read at 0x%" HWADDR_PRIx
                      "\n", __func__, offset);
        return 0;
    }
    return s->regs[reg];
}

static void aspeed_udc_ast2050_write(void *opaque, hwaddr offset, uint64_t data,
                                     unsigned size)
{
    AspeedUDCAST2050State *s = ASPEED_UDC_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_UDC_AST2050_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds write at 0x%" HWADDR_PRIx
                      "\n", __func__, offset);
        return;
    }
    s->regs[reg] = data;
}

static const MemoryRegionOps aspeed_udc_ast2050_ops = {
    .read = aspeed_udc_ast2050_read,
    .write = aspeed_udc_ast2050_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void aspeed_udc_ast2050_reset(DeviceState *dev)
{
    AspeedUDCAST2050State *s = ASPEED_UDC_AST2050(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void aspeed_udc_ast2050_realize(DeviceState *dev, Error **errp)
{
    AspeedUDCAST2050State *s = ASPEED_UDC_AST2050(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &aspeed_udc_ast2050_ops, s,
                          TYPE_ASPEED_UDC_AST2050,
                          ASPEED_UDC_AST2050_NR_REGS * 4);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription vmstate_aspeed_udc_ast2050 = {
    .name = TYPE_ASPEED_UDC_AST2050,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, AspeedUDCAST2050State,
                             ASPEED_UDC_AST2050_NR_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static void aspeed_udc_ast2050_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = aspeed_udc_ast2050_realize;
    device_class_set_legacy_reset(dc, aspeed_udc_ast2050_reset);
    dc->desc = "ASPEED AST2050 USB device/virtual-hub controller";
    dc->vmsd = &vmstate_aspeed_udc_ast2050;
}

static const TypeInfo aspeed_udc_ast2050_info = {
    .name = TYPE_ASPEED_UDC_AST2050,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AspeedUDCAST2050State),
    .class_init = aspeed_udc_ast2050_class_init,
};

static void aspeed_udc_ast2050_register_types(void)
{
    type_register_static(&aspeed_udc_ast2050_info);
}

type_init(aspeed_udc_ast2050_register_types)
