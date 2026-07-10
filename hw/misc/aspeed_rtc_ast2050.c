/*
 * ASPEED AST2050 (G3) Real-Time Clock — counter style (datasheet §24).
 *
 * RTC00 counter (R: sec/min/hour/day), RTC08 reload, RTC0C control ([0] enable),
 * RTC10 restart (write 0x5A loads the counter from reload), RTC14 reset (write
 * 0x99 clears). This is a register-accurate model of the load/read path; the 1 Hz
 * CLK32K counter advance is a deferred behavioural add-on (as with the PWM tach
 * RPM). See qemu-model/peripherals/rtc.
 *
 * This code is licensed under the GPL version 2 or later.
 */
#include "qemu/osdep.h"
#include "hw/misc/aspeed_rtc_ast2050.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define RTC_COUNTER 0x00
#define RTC_RELOAD  0x08
#define RTC_CONTROL 0x0C
#define RTC_RESTART 0x10
#define RTC_RESET   0x14
#define RESTART_MAGIC 0x5Au
#define RESET_MAGIC   0x99u

static uint64_t aspeed_rtc_ast2050_read(void *opaque, hwaddr offset, unsigned size)
{
    AspeedRtcAST2050State *s = ASPEED_RTC_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_RTC_AST2050_NR_REGS) {
        return 0;
    }
    return s->regs[reg];
}

static void aspeed_rtc_ast2050_write(void *opaque, hwaddr offset, uint64_t data,
                                     unsigned size)
{
    AspeedRtcAST2050State *s = ASPEED_RTC_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_RTC_AST2050_NR_REGS) {
        return;
    }
    switch (offset) {
    case RTC_COUNTER:
        /* counter status is read-only */
        break;
    case RTC_RESTART:
        /* magic 0x5A loads the counter from the reload register */
        if (data == RESTART_MAGIC) {
            s->regs[RTC_COUNTER >> 2] = s->regs[RTC_RELOAD >> 2];
        }
        break;
    case RTC_RESET:
        if (data == RESET_MAGIC) {
            memset(s->regs, 0, sizeof(s->regs));
        }
        break;
    default:
        s->regs[reg] = data;
        break;
    }
}

static const MemoryRegionOps aspeed_rtc_ast2050_ops = {
    .read = aspeed_rtc_ast2050_read,
    .write = aspeed_rtc_ast2050_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void aspeed_rtc_ast2050_reset(DeviceState *dev)
{
    AspeedRtcAST2050State *s = ASPEED_RTC_AST2050(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void aspeed_rtc_ast2050_realize(DeviceState *dev, Error **errp)
{
    AspeedRtcAST2050State *s = ASPEED_RTC_AST2050(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &aspeed_rtc_ast2050_ops, s,
                          TYPE_ASPEED_RTC_AST2050, 0x20);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription vmstate_aspeed_rtc_ast2050 = {
    .name = TYPE_ASPEED_RTC_AST2050,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, AspeedRtcAST2050State,
                             ASPEED_RTC_AST2050_NR_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static void aspeed_rtc_ast2050_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = aspeed_rtc_ast2050_realize;
    device_class_set_legacy_reset(dc, aspeed_rtc_ast2050_reset);
    dc->desc = "ASPEED AST2050 RTC (counter-style)";
    dc->vmsd = &vmstate_aspeed_rtc_ast2050;
}

static const TypeInfo aspeed_rtc_ast2050_info = {
    .name = TYPE_ASPEED_RTC_AST2050,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AspeedRtcAST2050State),
    .class_init = aspeed_rtc_ast2050_class_init,
};

static void aspeed_rtc_ast2050_register_types(void)
{
    type_register_static(&aspeed_rtc_ast2050_info);
}

type_init(aspeed_rtc_ast2050_register_types)
