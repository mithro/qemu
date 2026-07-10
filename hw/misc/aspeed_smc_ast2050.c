/*
 * ASPEED AST2050 (G3) legacy Static Memory Controller (SMC).
 *
 * Datasheet §11: SMC00 config (CE type + segment, reset 0x00000240 = CE0 NOR /
 * CE1 NAND / CE2 SPI, 32 MB segments), SMC04/08/0C per-CE control ([10:8] clock
 * divider, [5] MSB-first, [1:0] command mode), SMC10-1C misc / NAND ECC. Control
 * registers live at 0x16000000; the flash *data* windows (0x10000000 etc.) are a
 * separate AHB mapping and are not modelled here. This is the legacy SMC, NOT the
 * AST2400 FMC at 0x1E620000 that mainline aspeed_smc models.
 *
 * The AST2050 boots the OS from RAM (netboot), so the flash controller is only
 * probed; this register-accurate model presents the reset config and writable
 * control registers the firmware expects. Previously the vendor firmware's pokes
 * to this block hit unmapped MMIO and relied on the machine's tolerate-unmapped
 * flag; a real device here is the faithful behaviour.
 *
 * This code is licensed under the GPL version 2 or later.
 */
#include "qemu/osdep.h"
#include "hw/misc/aspeed_smc_ast2050.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

static uint64_t aspeed_smc_ast2050_read(void *opaque, hwaddr offset, unsigned size)
{
    AspeedSMCAST2050State *s = ASPEED_SMC_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_SMC_AST2050_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds read at 0x%" HWADDR_PRIx
                      "\n", __func__, offset);
        return 0;
    }
    return s->regs[reg];
}

static void aspeed_smc_ast2050_write(void *opaque, hwaddr offset, uint64_t data,
                                     unsigned size)
{
    AspeedSMCAST2050State *s = ASPEED_SMC_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_SMC_AST2050_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds write at 0x%" HWADDR_PRIx
                      "\n", __func__, offset);
        return;
    }
    s->regs[reg] = data;
}

static const MemoryRegionOps aspeed_smc_ast2050_ops = {
    .read = aspeed_smc_ast2050_read,
    .write = aspeed_smc_ast2050_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void aspeed_smc_ast2050_reset(DeviceState *dev)
{
    AspeedSMCAST2050State *s = ASPEED_SMC_AST2050(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[0] = ASPEED_SMC_AST2050_CONFIG_RESET;   /* SMC00 config */
}

static void aspeed_smc_ast2050_realize(DeviceState *dev, Error **errp)
{
    AspeedSMCAST2050State *s = ASPEED_SMC_AST2050(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &aspeed_smc_ast2050_ops, s,
                          TYPE_ASPEED_SMC_AST2050, 0x20);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_aspeed_smc_ast2050 = {
    .name = TYPE_ASPEED_SMC_AST2050,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, AspeedSMCAST2050State,
                             ASPEED_SMC_AST2050_NR_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static void aspeed_smc_ast2050_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = aspeed_smc_ast2050_realize;
    device_class_set_legacy_reset(dc, aspeed_smc_ast2050_reset);
    dc->desc = "ASPEED AST2050 legacy SMC (SPI flash controller)";
    dc->vmsd = &vmstate_aspeed_smc_ast2050;
}

static const TypeInfo aspeed_smc_ast2050_info = {
    .name = TYPE_ASPEED_SMC_AST2050,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AspeedSMCAST2050State),
    .class_init = aspeed_smc_ast2050_class_init,
};

static void aspeed_smc_ast2050_register_types(void)
{
    type_register_static(&aspeed_smc_ast2050_info);
}

type_init(aspeed_smc_ast2050_register_types)
