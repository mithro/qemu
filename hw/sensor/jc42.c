/*
 * JEDEC JC-42.4 DIMM temperature sensor (TS-on-DIMM / TSOD).
 *
 * Minimal-but-faithful model of the standard mobile-DRAM thermal sensor
 * found on DDR3 RDIMMs (e.g. Microchip MCP9805-class parts): eight 16-bit
 * big-endian registers behind a one-byte register pointer. Linux's jc42
 * hwmon driver accesses every register with i2c_smbus_{read,write}_word_
 * swapped(), i.e. MSB on the wire first - this model matches that ordering.
 *
 * Registers (JC-42.4):
 *   0x00 capabilities   0x01 configuration  0x02 alarm upper
 *   0x03 alarm lower    0x04 critical       0x05 temperature (RO)
 *   0x06 manufacturer ID (RO)               0x07 device ID/revision (RO)
 *
 * The temperature register encodes 0.0625 C/LSB two's-complement in bits
 * [12:0] with trip-status flags in [15:13]; the "temperature" QOM property
 * (millidegrees C, like tmp105) drives it and recomputes the trip flags
 * against the limit registers.
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

#define TYPE_JC42 "jc42"
OBJECT_DECLARE_SIMPLE_TYPE(JC42State, JC42)

#define JC42_REG_CAP        0x00
#define JC42_REG_CONFIG     0x01
#define JC42_REG_UPPER      0x02
#define JC42_REG_LOWER      0x03
#define JC42_REG_CRITICAL   0x04
#define JC42_REG_TEMP       0x05
#define JC42_REG_MANID      0x06
#define JC42_REG_DEVID      0x07
#define JC42_NR_REGS        8

/*
 * Capabilities: EVENT + TCRIT support, +/-1 C accuracy class, 0.0625 C
 * resolution (bits 4:3 = 0b11), -40..+125 C range - the common RDIMM part
 * feature set the Linux driver keys resolution off.
 */
#define JC42_CAP_DEFAULT    0x00ff
/*
 * Microchip MCP98244 (DIMM TS): manufacturer 0x0054, device ID 0x2200 --
 * present in Linux jc42.c's ID table (MCP98244_DEVID, mask 0xfffc), so both
 * DT probe and I2C auto-detect accept the model. The detect() path also
 * requires (cap & 0xff00) == 0, which JC42_CAP_DEFAULT satisfies.
 */
#define JC42_MANID_MCHP     0x0054
#define JC42_DEVID_MCP98244 0x2200

#define JC42_TEMP_FLAG_CRIT   (1u << 15)
#define JC42_TEMP_FLAG_ABOVE  (1u << 14)
#define JC42_TEMP_FLAG_BELOW  (1u << 13)
#define JC42_TEMP_SIGN        (1u << 12)

typedef struct JC42State {
    SMBusDevice parent_obj;

    uint16_t regs[JC42_NR_REGS];
    uint8_t pointer;
    uint8_t rx_count;

    int32_t temperature_mC;
} JC42State;

/* Convert millidegrees C to the 0.0625 C/LSB 13-bit two's complement field. */
static uint16_t jc42_encode_temp(int32_t mC)
{
    int32_t units = mC * 16 / 1000;

    return (uint16_t)(units & 0x1fff);
}

/* Sign-extend a 13-bit register field to a signed value in units. */
static int32_t jc42_field_units(uint16_t field)
{
    int32_t v = field & 0x1fff;

    return (v & JC42_TEMP_SIGN) ? v - 0x2000 : v;
}

static void jc42_update_temp(JC42State *s)
{
    uint16_t enc = jc42_encode_temp(s->temperature_mC);
    int32_t t = jc42_field_units(enc);
    uint16_t flags = 0;

    if (t >= jc42_field_units(s->regs[JC42_REG_CRITICAL])) {
        flags |= JC42_TEMP_FLAG_CRIT;
    }
    if (t > jc42_field_units(s->regs[JC42_REG_UPPER])) {
        flags |= JC42_TEMP_FLAG_ABOVE;
    }
    if (t < jc42_field_units(s->regs[JC42_REG_LOWER])) {
        flags |= JC42_TEMP_FLAG_BELOW;
    }

    s->regs[JC42_REG_TEMP] = flags | enc;
}

static int jc42_write_data(SMBusDevice *dev, uint8_t *buf, uint8_t len)
{
    JC42State *s = JC42(dev);

    if (len == 0) {
        return 0;
    }

    s->pointer = buf[0] & (JC42_NR_REGS - 1);
    s->rx_count = 0;

    if (len == 3) {
        /* Register write: MSB first on the wire (word-swapped). */
        uint16_t val = (buf[1] << 8) | buf[2];

        switch (s->pointer) {
        case JC42_REG_CONFIG:
        case JC42_REG_UPPER:
        case JC42_REG_LOWER:
        case JC42_REG_CRITICAL:
            s->regs[s->pointer] = val;
            jc42_update_temp(s);
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: write to read-only register 0x%02x\n",
                          __func__, s->pointer);
            break;
        }
    } else if (len != 1) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: unexpected %u-byte write\n", __func__, len);
        return -1;
    }

    return 0;
}

static uint8_t jc42_receive_byte(SMBusDevice *dev)
{
    JC42State *s = JC42(dev);
    uint16_t val = s->regs[s->pointer];

    /* MSB first, matching i2c_smbus_read_word_swapped() expectations. */
    if (s->rx_count++ % 2 == 0) {
        return val >> 8;
    }
    return val & 0xff;
}

static void jc42_get_temperature(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    JC42State *s = JC42(obj);

    visit_type_int32(v, name, &s->temperature_mC, errp);
}

static void jc42_set_temperature(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    JC42State *s = JC42(obj);
    int32_t value;

    if (!visit_type_int32(v, name, &value, errp)) {
        return;
    }
    if (value < -40000 || value > 125000) {
        error_setg(errp, "jc42: temperature %d out of range", value);
        return;
    }

    s->temperature_mC = value;
    jc42_update_temp(s);
}

static void jc42_reset_hold(Object *obj, ResetType type)
{
    JC42State *s = JC42(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[JC42_REG_CAP] = JC42_CAP_DEFAULT;
    s->regs[JC42_REG_MANID] = JC42_MANID_MCHP;
    s->regs[JC42_REG_DEVID] = JC42_DEVID_MCP98244;
    s->pointer = 0;
    s->rx_count = 0;
    jc42_update_temp(s);
}

static const VMStateDescription vmstate_jc42 = {
    .name = TYPE_JC42,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SMBUS_DEVICE(parent_obj, JC42State),
        VMSTATE_UINT16_ARRAY(regs, JC42State, JC42_NR_REGS),
        VMSTATE_UINT8(pointer, JC42State),
        VMSTATE_UINT8(rx_count, JC42State),
        VMSTATE_INT32(temperature_mC, JC42State),
        VMSTATE_END_OF_LIST()
    }
};

static void jc42_init(Object *obj)
{
    JC42State *s = JC42(obj);

    /* Default: a warm-but-healthy DIMM. */
    s->temperature_mC = 35000;

    object_property_add(obj, "temperature", "int32",
                        jc42_get_temperature, jc42_set_temperature,
                        NULL, NULL);
}

static void jc42_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    SMBusDeviceClass *k = SMBUS_DEVICE_CLASS(klass);

    dc->desc = "JEDEC JC-42.4 DIMM temperature sensor (TSOD)";
    dc->vmsd = &vmstate_jc42;
    rc->phases.hold = jc42_reset_hold;
    k->write_data = jc42_write_data;
    k->receive_byte = jc42_receive_byte;
}

static const TypeInfo jc42_info = {
    .name          = TYPE_JC42,
    .parent        = TYPE_SMBUS_DEVICE,
    .instance_size = sizeof(JC42State),
    .instance_init = jc42_init,
    .class_init    = jc42_class_init,
};

static void jc42_register_types(void)
{
    type_register_static(&jc42_info);
}

type_init(jc42_register_types)
