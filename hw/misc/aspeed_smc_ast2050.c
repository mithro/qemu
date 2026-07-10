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
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"

/*
 * SPI chip-select is asserted (flash selected) when a CE control register
 * (SMC04/08/0C) is in user mode (bits[1:0]=3) AND its CE# bit (bit 2) is clear:
 * the vendor's cs_low writes 0x3 (mode=user, CE# active) and cs_high writes 0x7
 * (mode=user, CE# inactive). So the select condition is (reg & 0x7) == 0x3.
 */
static bool ast2050_smc_cs_asserted(AspeedSMCAST2050State *s)
{
    unsigned ce;

    for (ce = 1; ce <= 3; ce++) {
        if ((s->regs[ce] & 0x7) == 0x3) {
            return true;
        }
    }
    return false;
}

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

    /*
     * A write to a CE control register (SMC04/08/0C) may change the chip-select:
     * (reg & 0x7)==0x3 selects the flash (user mode + CE# active, cs_low writes
     * 0x3), otherwise it is deselected (cs_high writes 0x7, ending the SPI command
     * and resetting the flash command state). SSI CS is active per "unselect":
     * qemu_set_irq(cs_line, 0) selects, 1 deselects.
     */
    if (reg >= 1 && reg <= 3) {
        bool sel = ast2050_smc_cs_asserted(s);
        if (sel != s->cs_asserted) {
            s->cs_asserted = sel;
            qemu_set_irq(s->cs_line, sel ? 0 : 1);
        }
    }
}

static const MemoryRegionOps aspeed_smc_ast2050_ops = {
    .read = aspeed_smc_ast2050_read,
    .write = aspeed_smc_ast2050_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/*
 * Flash window (0x10000000..0x16000000, the CE0-CE2 data windows). In UMA/user
 * mode each byte access is one SPI byte on the wire: strb <cmd/data> clocks a
 * byte out, ldrb clocks a byte in (dummy 0xff). This is how the vendor reads the
 * JEDEC ID (0x9F -> C2 20 18) and how ast2050_smc_uma_read/write byte-bang (the
 * window base per CE comes from ast2050_get_baddr: 0x10000000 / 0x12000000). In
 * non-user (normal read) mode the flash-content mapping is not modelled (the
 * AST2050 netboots from RAM); such reads return 0, matching the previous
 * tolerate-unmapped behaviour.
 */
static uint64_t aspeed_smc_ast2050_flash_read(void *opaque, hwaddr offset,
                                              unsigned size)
{
    AspeedSMCAST2050State *s = ASPEED_SMC_AST2050(opaque);
    uint64_t r = 0;
    unsigned i;

    if (!ast2050_smc_cs_asserted(s)) {
        return 0;
    }
    for (i = 0; i < size; i++) {
        r |= (uint64_t)(ssi_transfer(s->spi, 0xff) & 0xff) << (8 * i);
    }
    return r;
}

static void aspeed_smc_ast2050_flash_write(void *opaque, hwaddr offset,
                                           uint64_t data, unsigned size)
{
    AspeedSMCAST2050State *s = ASPEED_SMC_AST2050(opaque);
    unsigned i;

    if (!ast2050_smc_cs_asserted(s)) {
        return;
    }
    for (i = 0; i < size; i++) {
        ssi_transfer(s->spi, (data >> (8 * i)) & 0xff);
    }
}

static const MemoryRegionOps aspeed_smc_ast2050_flash_ops = {
    .read = aspeed_smc_ast2050_flash_read,
    .write = aspeed_smc_ast2050_flash_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void aspeed_smc_ast2050_reset(DeviceState *dev)
{
    AspeedSMCAST2050State *s = ASPEED_SMC_AST2050(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[0] = ASPEED_SMC_AST2050_CONFIG_RESET;   /* SMC00 config */
    s->cs_asserted = false;
    qemu_set_irq(s->cs_line, 1);                    /* deselect the flash */
}

static void aspeed_smc_ast2050_realize(DeviceState *dev, Error **errp)
{
    AspeedSMCAST2050State *s = ASPEED_SMC_AST2050(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    DeviceState *flash;

    /* SPI bus + the CE2 NOR flash (Macronix MX25L12805D, JEDEC 0xC22018). */
    s->spi = ssi_create_bus(dev, "spi");
    flash = qdev_new("mx25l12805d");
    if (!qdev_realize_and_unref(flash, BUS(s->spi), errp)) {
        return;
    }
    s->cs_line = qdev_get_gpio_in_named(flash, SSI_GPIO_CS, 0);

    memory_region_init_io(&s->iomem, OBJECT(s), &aspeed_smc_ast2050_ops, s,
                          TYPE_ASPEED_SMC_AST2050, 0x20);
    sysbus_init_mmio(sbd, &s->iomem);          /* mmio[0] -> 0x16000000 ctrl  */

    memory_region_init_io(&s->flash_window, OBJECT(s),
                          &aspeed_smc_ast2050_flash_ops, s,
                          "aspeed.smc-ast2050.flash-window",
                          ASPEED_SMC_AST2050_WINDOW_SIZE);
    sysbus_init_mmio(sbd, &s->flash_window);   /* mmio[1] -> 0x10000000 window */
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
