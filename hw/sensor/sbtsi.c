/*
 * AMD SB-TSI (Side-Band Temperature Sensor Interface).
 *
 * Faithful model of the AMD processor's SB-TSI thermal interface as reached by
 * the ASUS KGPE-D16 BMC on I2C4 (schematic §10.2: "AMD SB-TSI 0x4C/0x4D", via
 * the QU4 level-shift FETs Q56-Q59). Register layout matches the Linux
 * `sbtsi_temp` hwmon driver (drivers/hwmon/sbtsi_temp.c):
 *
 *   0x01 TEMP_INT  (RO)  integer CPU temperature, 0..255 C
 *   0x02 STATUS    (RO)  alarm/status bits
 *   0x03 CONFIG    (RO)  bit5 = read-order (int-first vs dec-first)
 *   0x07 TEMP_HIGH_INT (RW)  high-limit integer
 *   0x08 TEMP_LOW_INT  (RW)  low-limit integer
 *   0x10 TEMP_DEC  (RW)  bits[7:5] = fractional temp in 0.125 C steps
 *   0x13 TEMP_HIGH_DEC (RW)  high-limit fractional
 *   0x14 TEMP_LOW_DEC  (RW)  low-limit fractional
 *
 * Temperature = TEMP_INT*1000 + (TEMP_DEC>>5)*125 millidegrees C. The "temperature"
 * QOM property (millidegrees, like tmp105/jc42) drives TEMP_INT/TEMP_DEC.
 *
 * Copyright 2026, Apache-2.0 OR GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/smbus_slave.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_SBTSI "sbtsi"
OBJECT_DECLARE_SIMPLE_TYPE(SBTSIState, SBTSI)

#define SBTSI_REG_TEMP_INT       0x01
#define SBTSI_REG_STATUS         0x02
#define SBTSI_REG_CONFIG         0x03
#define SBTSI_REG_TEMP_HIGH_INT  0x07
#define SBTSI_REG_TEMP_LOW_INT   0x08
#define SBTSI_REG_TEMP_DEC       0x10
#define SBTSI_REG_TEMP_HIGH_DEC  0x13
#define SBTSI_REG_TEMP_LOW_DEC   0x14
#define SBTSI_NR_REGS            0x15   /* 0x00..0x14 */

/* Fractional temperature lives in bits [7:5] of TEMP_DEC (0.125 C/step). */
#define SBTSI_TEMP_DEC_SHIFT     5
#define SBTSI_TEMP_DEC_MASK      (0x7u << SBTSI_TEMP_DEC_SHIFT)

typedef struct SBTSIState {
    SMBusDevice parent_obj;

    uint8_t regs[SBTSI_NR_REGS];
    uint8_t pointer;

    int32_t temperature_mC;   /* millidegrees C, 0..255875 */
} SBTSIState;

static void sbtsi_update_temp(SBTSIState *s)
{
    int32_t t = s->temperature_mC;

    if (t < 0) {
        t = 0;
    } else if (t > 255875) {
        t = 255875;
    }
    s->regs[SBTSI_REG_TEMP_INT] = (uint8_t)(t / 1000);
    s->regs[SBTSI_REG_TEMP_DEC] =
        (uint8_t)(((t % 1000) / 125) << SBTSI_TEMP_DEC_SHIFT);
}

static bool sbtsi_writable(uint8_t idx)
{
    switch (idx) {
    case SBTSI_REG_TEMP_HIGH_INT:
    case SBTSI_REG_TEMP_LOW_INT:
    case SBTSI_REG_TEMP_HIGH_DEC:
    case SBTSI_REG_TEMP_LOW_DEC:
        return true;
    default:
        /* TEMP_INT/STATUS/CONFIG are RO; TEMP_DEC is nominally RW but is
         * driven by the temperature property, so treat it RO here. */
        return false;
    }
}

static int sbtsi_write_data(SMBusDevice *dev, uint8_t *buf, uint8_t len)
{
    SBTSIState *s = SBTSI(dev);
    unsigned i;

    if (len == 0) {
        return 0;
    }
    s->pointer = buf[0];

    for (i = 1; i < len; i++) {
        uint8_t idx = s->pointer + (i - 1);

        if (idx < SBTSI_NR_REGS && sbtsi_writable(idx)) {
            s->regs[idx] = buf[i];
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: write 0x%02x to RO/reserved reg 0x%02x ignored\n",
                          __func__, buf[i], idx);
        }
    }
    if (len > 1) {
        s->pointer += (len - 1);
    }
    return 0;
}

static uint8_t sbtsi_receive_byte(SMBusDevice *dev)
{
    SBTSIState *s = SBTSI(dev);
    uint8_t idx = s->pointer;
    uint8_t val = (idx < SBTSI_NR_REGS) ? s->regs[idx] : 0xff;

    s->pointer = idx + 1;
    return val;
}

static void sbtsi_get_temperature(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    SBTSIState *s = SBTSI(obj);

    visit_type_int32(v, name, &s->temperature_mC, errp);
}

static void sbtsi_set_temperature(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    SBTSIState *s = SBTSI(obj);
    int32_t value;

    if (!visit_type_int32(v, name, &value, errp)) {
        return;
    }
    if (value < 0 || value > 255875) {
        error_setg(errp, "sbtsi: temperature %d out of range (0..255875)",
                   value);
        return;
    }
    s->temperature_mC = value;
    sbtsi_update_temp(s);
}

static void sbtsi_reset_hold(Object *obj, ResetType type)
{
    SBTSIState *s = SBTSI(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[SBTSI_REG_CONFIG] = 0x00;            /* read-order: int first */
    s->regs[SBTSI_REG_TEMP_HIGH_INT] = 0xff;     /* high limit = max */
    s->regs[SBTSI_REG_TEMP_LOW_INT] = 0x00;
    s->pointer = 0;
    sbtsi_update_temp(s);
}

static const VMStateDescription vmstate_sbtsi = {
    .name = TYPE_SBTSI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SMBUS_DEVICE(parent_obj, SBTSIState),
        VMSTATE_UINT8_ARRAY(regs, SBTSIState, SBTSI_NR_REGS),
        VMSTATE_UINT8(pointer, SBTSIState),
        VMSTATE_INT32(temperature_mC, SBTSIState),
        VMSTATE_END_OF_LIST()
    }
};

static void sbtsi_init(Object *obj)
{
    SBTSIState *s = SBTSI(obj);

    /* Default: a warm-but-healthy AMD CPU, 45.5 C. */
    s->temperature_mC = 45500;

    object_property_add(obj, "temperature", "int32",
                        sbtsi_get_temperature, sbtsi_set_temperature,
                        NULL, NULL);
}

static void sbtsi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    SMBusDeviceClass *k = SMBUS_DEVICE_CLASS(klass);

    dc->desc = "AMD SB-TSI processor thermal sensor";
    dc->vmsd = &vmstate_sbtsi;
    rc->phases.hold = sbtsi_reset_hold;
    k->write_data = sbtsi_write_data;
    k->receive_byte = sbtsi_receive_byte;
}

static const TypeInfo sbtsi_info = {
    .name          = TYPE_SBTSI,
    .parent        = TYPE_SMBUS_DEVICE,
    .instance_size = sizeof(SBTSIState),
    .instance_init = sbtsi_init,
    .class_init    = sbtsi_class_init,
};

static void sbtsi_register_types(void)
{
    type_register_static(&sbtsi_info);
}

type_init(sbtsi_register_types)
