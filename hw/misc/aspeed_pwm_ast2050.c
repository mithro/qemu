/*
 * ASPEED AST2050 (G3) PWM & Tachometer controller.
 *
 * Datasheet §28: PTCR00 general control ([0] master clock, [11:8] PWM A-D enable,
 * [31:16] tach enable), PTCR08/0C duty (rise/fall), PTCR2C tach result (R: [31]
 * full, [19:0] value). RPM = (24MHz*60)/(2*TachoValue*TachoClkDiv). The G3 has
 * exactly 4 PWM + 16 tach and no register at/above 0x40 (unlike newer parts).
 *
 * This is a register-accurate model (RW registers; the result register is
 * read-only). The tach RPM computation from the programmed duty is a refinement;
 * OpenBMC's aspeed hwmon driver binds and reads/writes the control + duty here.
 *
 * This code is licensed under the GPL version 2 or later.
 */
#include "qemu/osdep.h"
#include "hw/misc/aspeed_pwm_ast2050.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define PTCR_RESULT 0x2C   /* read-only tach result */

static uint64_t aspeed_pwm_ast2050_read(void *opaque, hwaddr offset, unsigned size)
{
    AspeedPWMAST2050State *s = ASPEED_PWM_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_PWM_AST2050_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds read at 0x%" HWADDR_PRIx
                      "\n", __func__, offset);
        return 0;
    }
    return s->regs[reg];
}

static void aspeed_pwm_ast2050_write(void *opaque, hwaddr offset, uint64_t data,
                                     unsigned size)
{
    AspeedPWMAST2050State *s = ASPEED_PWM_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_PWM_AST2050_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds write at 0x%" HWADDR_PRIx
                      "\n", __func__, offset);
        return;
    }
    if (reg == (PTCR_RESULT >> 2)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to read-only tach result\n",
                      __func__);
        return;
    }
    s->regs[reg] = data;
}

static const MemoryRegionOps aspeed_pwm_ast2050_ops = {
    .read = aspeed_pwm_ast2050_read,
    .write = aspeed_pwm_ast2050_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void aspeed_pwm_ast2050_reset(DeviceState *dev)
{
    AspeedPWMAST2050State *s = ASPEED_PWM_AST2050(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void aspeed_pwm_ast2050_realize(DeviceState *dev, Error **errp)
{
    AspeedPWMAST2050State *s = ASPEED_PWM_AST2050(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &aspeed_pwm_ast2050_ops, s,
                          TYPE_ASPEED_PWM_AST2050, 0x40);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription vmstate_aspeed_pwm_ast2050 = {
    .name = TYPE_ASPEED_PWM_AST2050,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, AspeedPWMAST2050State,
                             ASPEED_PWM_AST2050_NR_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static void aspeed_pwm_ast2050_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = aspeed_pwm_ast2050_realize;
    device_class_set_legacy_reset(dc, aspeed_pwm_ast2050_reset);
    dc->desc = "ASPEED AST2050 PWM/Tachometer";
    dc->vmsd = &vmstate_aspeed_pwm_ast2050;
}

static const TypeInfo aspeed_pwm_ast2050_info = {
    .name = TYPE_ASPEED_PWM_AST2050,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AspeedPWMAST2050State),
    .class_init = aspeed_pwm_ast2050_class_init,
};

static void aspeed_pwm_ast2050_register_types(void)
{
    type_register_static(&aspeed_pwm_ast2050_info);
}

type_init(aspeed_pwm_ast2050_register_types)
