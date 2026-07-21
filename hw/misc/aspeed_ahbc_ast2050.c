/*
 * ASPEED AST2050 (G3) AHB Bus Controller (AHBC).
 *
 * Faithful register model of §12 (base 0x1E600000) plus the boot-remap: a write
 * to AHBC8C toggles the SDRAM-low alias (remap_mr) that the SoC maps at 0x0. The
 * alias is default-disabled (reset = boot from static memory, §12.3 p115), so no
 * firmware oracle that leaves AHBC8C[0]=0 sees any memory-map change. Enabling it
 * makes the low 256 MB aperture SDRAM, which is how the 28-bit MDMA engine (§22)
 * reaches DRAM.
 *
 * This code is licensed under the GPL version 2 or later.
 */
#include "qemu/osdep.h"
#include "hw/misc/aspeed_ahbc_ast2050.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define AHBC_REMAP       0x8C
#define AHBC_REMAP_SDRAM BIT(0)   /* 0: static memory at 0x0, 1: SDRAM at 0x0 */

static void aspeed_ahbc_set_remap(AspeedAHBCAST2050State *s, bool sdram)
{
    if (s->remap_mr) {
        memory_region_set_enabled(s->remap_mr, sdram);
    }
}

static uint64_t aspeed_ahbc_read(void *opaque, hwaddr offset, unsigned size)
{
    AspeedAHBCAST2050State *s = ASPEED_AHBC_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_AHBC_AST2050_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds read at 0x%" HWADDR_PRIx
                      "\n", __func__, offset);
        return 0;
    }
    return s->regs[reg];
}

static void aspeed_ahbc_write(void *opaque, hwaddr offset, uint64_t data,
                              unsigned size)
{
    AspeedAHBCAST2050State *s = ASPEED_AHBC_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_AHBC_AST2050_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds write at 0x%" HWADDR_PRIx
                      "\n", __func__, offset);
        return;
    }

    s->regs[reg] = data;

    if (offset == AHBC_REMAP) {
        aspeed_ahbc_set_remap(s, data & AHBC_REMAP_SDRAM);
    }
}

static const MemoryRegionOps aspeed_ahbc_ops = {
    .read = aspeed_ahbc_read,
    .write = aspeed_ahbc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void aspeed_ahbc_reset(DeviceState *dev)
{
    AspeedAHBCAST2050State *s = ASPEED_AHBC_AST2050(dev);

    memset(s->regs, 0, sizeof(s->regs));
    /* Reset default: AHBC8C[0]=0 -> boot from static memory (alias disabled). */
    aspeed_ahbc_set_remap(s, false);
}

static void aspeed_ahbc_realize(DeviceState *dev, Error **errp)
{
    AspeedAHBCAST2050State *s = ASPEED_AHBC_AST2050(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &aspeed_ahbc_ops, s,
                          TYPE_ASPEED_AHBC_AST2050, 0x90);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_aspeed_ahbc_ast2050 = {
    .name = TYPE_ASPEED_AHBC_AST2050,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, AspeedAHBCAST2050State,
                             ASPEED_AHBC_AST2050_NR_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static void aspeed_ahbc_ast2050_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = aspeed_ahbc_realize;
    device_class_set_legacy_reset(dc, aspeed_ahbc_reset);
    dc->desc = "ASPEED AST2050 AHB Bus Controller";
    dc->vmsd = &vmstate_aspeed_ahbc_ast2050;
}

static const TypeInfo aspeed_ahbc_ast2050_info = {
    .name = TYPE_ASPEED_AHBC_AST2050,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AspeedAHBCAST2050State),
    .class_init = aspeed_ahbc_ast2050_class_init,
};

static void aspeed_ahbc_ast2050_register_types(void)
{
    type_register_static(&aspeed_ahbc_ast2050_info);
}

type_init(aspeed_ahbc_ast2050_register_types)
