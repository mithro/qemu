/*
 * Nuvoton/Winbond W83601G — SMBus (I2C) GPI/O expander.
 *
 * Faithful model of the 20-pin SSOP GPIO expander used on the ASUS KGPE-D16
 * (U27/U28) to drive the DIMM error LEDs off the BMC's I2C5 engine. Register
 * map, resets, read-only set and the 0b0011_A2A1A0 (0x18..0x1F) address are
 * taken from the official Nuvoton datasheet V1.31 (Mar 2009); see
 * asus-kgpe-d16-firmware/openbmc/bmc-functionality/evidence/d08-w83601g/.
 *
 * Access is the classic register-pointer SMBus device: a write transfer's
 * first byte is the CR index, subsequent bytes write (auto-incrementing) into
 * the register file; a read transfer returns the register at the pointer,
 * auto-incrementing. Read-only and reserved indices ignore writes; reserved
 * indices read back 0xFF (open bus, matching silicon: CR07 -> 0xFF).
 *
 * The two Port-1/Port-2 output-data registers (CR01 / CR09) are the ones the
 * BMC writes to light DIMM{A..H}ERRLED. Port I/O-config resets to all-inputs
 * (CR03=0xFF, CR0B=0x7F) so firmware must clear the direction bit before the
 * output data takes effect on a pin - modelled exactly.
 *
 * Copyright 2026, Apache-2.0 OR GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/smbus_slave.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_W83601G "w83601g"
OBJECT_DECLARE_SIMPLE_TYPE(W83601GState, W83601G)

#define W83601G_NR_REGS     0x22    /* CR00..CR21 */

/* Register indices (datasheet §7.1). */
#define CR_P1_IN            0x00
#define CR_P1_OUT           0x01
#define CR_P1_POLARITY      0x02
#define CR_P1_IOCFG         0x03
#define CR_P1_OUTSTYLE      0x04
#define CR_P1_IN_LATCH      0x05
#define CR_P2_IN            0x08
#define CR_P2_OUT           0x09
#define CR_P2_POLARITY      0x0a
#define CR_P2_IOCFG         0x0b
#define CR_P2_OUTSTYLE      0x0c
#define CR_P2_IN_LATCH      0x0d
#define CR_P1_INT_STATUS    0x10
#define CR_P2_INT_STATUS    0x11
#define CR_P1_INT_ENABLE    0x12
#define CR_P2_INT_ENABLE    0x13
#define CR_MODE_CFG         0x14
#define CR_PWRLED_CFG       0x15
#define CR_ID_HIGH          0x20
#define CR_ID_LOW           0x21

#define W83601G_ID_HIGH     0x60
/*
 * CR21 chip-ID low. The datasheet is self-inconsistent (§7.1 table says 0x12,
 * §7.2 text says 0x13); this rig's silicon reads 0x13 (evidence/d08-w83601g/
 * 03-silicon-both-sides.txt), so model the silicon truth per the "QEMU models
 * real hardware" rule.
 */
#define W83601G_ID_LOW      0x13

typedef struct W83601GState {
    SMBusDevice parent_obj;

    uint8_t regs[W83601G_NR_REGS];
    uint8_t pointer;

    /* Port input-data latch values, board/strap dependent -> seeded per
     * instance from silicon captures so QEMU matches the real board. */
    uint8_t port1_input;
    uint8_t port2_input;
} W83601GState;

/*
 * Writable bitmap: 1 => the CR is R/W, 0 => read-only or reserved (writes
 * ignored). Indices not listed here (reserved: 06,07,0e,0f,16-1f and the RO
 * inputs/IDs) are read-only.
 */
static bool w83601g_writable(uint8_t idx)
{
    switch (idx) {
    case CR_P1_OUT:
    case CR_P1_POLARITY:
    case CR_P1_IOCFG:
    case CR_P1_OUTSTYLE:
    case CR_P2_OUT:
    case CR_P2_POLARITY:
    case CR_P2_IOCFG:
    case CR_P2_OUTSTYLE:
    case CR_P1_INT_ENABLE:
    case CR_P2_INT_ENABLE:
    case CR_MODE_CFG:
    case CR_PWRLED_CFG:
        return true;
    default:
        return false;
    }
}

/*
 * A reserved index is one with no defined register (an internal hole, or any
 * index past the last register CR21); it reads back 0xFF and ignores writes.
 * NB: the register count (0x22) is not a power of two, so callers must range-
 * check, never mask, the pointer.
 */
static bool w83601g_reserved(uint8_t idx)
{
    if (idx >= W83601G_NR_REGS) {
        return true;
    }
    switch (idx) {
    case 0x06: case 0x07: case 0x0e: case 0x0f:
    case 0x16: case 0x17: case 0x18: case 0x19:
    case 0x1a: case 0x1b: case 0x1c: case 0x1d:
    case 0x1e: case 0x1f:
        return true;
    default:
        return false;
    }
}

static int w83601g_write_data(SMBusDevice *dev, uint8_t *buf, uint8_t len)
{
    W83601GState *s = W83601G(dev);
    unsigned i;

    if (len == 0) {
        return 0;
    }

    s->pointer = buf[0];   /* 8-bit index; wraps mod 256 like the silicon */

    /* buf[1..] are data bytes written at the pointer, auto-incrementing. */
    for (i = 1; i < len; i++) {
        uint8_t idx = s->pointer + (i - 1);   /* uint8_t wraps at 256 */

        if (w83601g_writable(idx)) {
            s->regs[idx] = buf[i];
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: write 0x%02x to read-only/reserved CR%02x "
                          "ignored\n", __func__, buf[i], idx);
        }
    }
    if (len > 1) {
        s->pointer += (len - 1);
    }
    return 0;
}

static uint8_t w83601g_receive_byte(SMBusDevice *dev)
{
    W83601GState *s = W83601G(dev);
    uint8_t idx = s->pointer;
    uint8_t val;

    if (w83601g_reserved(idx)) {
        val = 0xff;   /* hole or out-of-range index: open bus (== silicon) */
    } else {
        val = s->regs[idx];
    }
    s->pointer = idx + 1;   /* uint8_t wraps at 256 */
    return val;
}

static void w83601g_reset_hold(Object *obj, ResetType type)
{
    W83601GState *s = W83601G(obj);

    memset(s->regs, 0, sizeof(s->regs));

    /* Datasheet §7.1 reset defaults. */
    s->regs[CR_P1_IN]        = s->port1_input;
    s->regs[CR_P1_OUT]       = 0x00;
    s->regs[CR_P1_POLARITY]  = 0xf0;
    s->regs[CR_P1_IOCFG]     = 0xff;   /* all inputs */
    s->regs[CR_P1_OUTSTYLE]  = 0x00;
    s->regs[CR_P1_IN_LATCH]  = s->port1_input;
    s->regs[CR_P2_IN]        = s->port2_input;
    s->regs[CR_P2_OUT]       = 0x00;
    s->regs[CR_P2_POLARITY]  = 0x70;
    s->regs[CR_P2_IOCFG]     = 0x7f;   /* all inputs */
    s->regs[CR_P2_OUTSTYLE]  = 0x00;
    s->regs[CR_P2_IN_LATCH]  = s->port2_input;
    s->regs[CR_ID_HIGH]      = W83601G_ID_HIGH;
    s->regs[CR_ID_LOW]       = W83601G_ID_LOW;

    s->pointer = 0;
}

static const VMStateDescription vmstate_w83601g = {
    .name = TYPE_W83601G,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SMBUS_DEVICE(parent_obj, W83601GState),
        VMSTATE_UINT8_ARRAY(regs, W83601GState, W83601G_NR_REGS),
        VMSTATE_UINT8(pointer, W83601GState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property w83601g_props[] = {
    DEFINE_PROP_UINT8("port1-input", W83601GState, port1_input, 0x00),
    DEFINE_PROP_UINT8("port2-input", W83601GState, port2_input, 0x00),
};

static void w83601g_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    SMBusDeviceClass *k = SMBUS_DEVICE_CLASS(klass);

    dc->desc = "Nuvoton W83601G SMBus GPIO expander (DIMM-LED)";
    dc->vmsd = &vmstate_w83601g;
    device_class_set_props(dc, w83601g_props);
    rc->phases.hold = w83601g_reset_hold;
    k->write_data = w83601g_write_data;
    k->receive_byte = w83601g_receive_byte;
}

static const TypeInfo w83601g_info = {
    .name          = TYPE_W83601G,
    .parent        = TYPE_SMBUS_DEVICE,
    .instance_size = sizeof(W83601GState),
    .class_init    = w83601g_class_init,
};

static void w83601g_register_types(void)
{
    type_register_static(&w83601g_info);
}

type_init(w83601g_register_types)
