/*
 * Nuvoton/Winbond W83795G hardware-monitor (I2C).
 *
 * Faithful-enough model of the W83795G "H/W monitoring IC" as driven by the
 * mainline Linux driver drivers/hwmon/w83795.c: 21 voltage inputs, 6 analog
 * temperatures (thermal-diode / thermistor), 2 DTS (AMD SB-TSI) temperatures,
 * 8 fan-tach inputs and 8 PWM outputs, all reached through the chip's
 * bank-switched register file.
 *
 * This models the register-level behaviour the driver relies on:
 *   - a Bank-Select register at index 0x00 (low 3 bits pick the bank; bit 7
 *     flips the vendor-ID readback, exactly what w83795_detect() checks),
 *   - the identification registers (vendor 0xA3/0x5C, chip-id 0x79, device-id
 *     0x50 = rev A) so both detect() and an explicit "w83795g" instantiation
 *     bind cleanly,
 *   - the "which channels are present" control registers, pre-loaded with the
 *     ASUS KGPE-D16 configuration taken from coreboot's
 *     src/mainboard/asus/kgpe-d16/devicetree.cb (fanin_ctl1=0xff, volt_ctl1=
 *     0xff/volt_ctl2=0xf7, temp_ctl1=0x2a/temp_ctl2=0x01, temp_dtse=0x03), so
 *     the driver enables fan1-8, the populated voltage rails, the CPU thermal
 *     diode and the two per-socket DTS die temperatures,
 *   - the measurement registers, pre-loaded with plausible readings for a
 *     running dual-Opteron board, and
 *   - the shared VRLSB register (0x3C): on real silicon this latches the low
 *     bits of the most-recently-read measurement register; the driver reads a
 *     measurement high byte and then VRLSB, so we shadow the last read.
 *
 * Wired onto the kgpe-d16-bmc machine's I2C bus 1 at address 0x2f, matching the
 * real board (BMC I2C bus 1 / 0x2f); see
 * asus-kgpe-d16-firmware/openbmc/bmc-functionality/HW-WIRING-power-sensors.md.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

/* --- register map (matches drivers/hwmon/w83795.c) --- */
#define W83795_REG_BANKSEL      0x00
#define W83795_REG_CONFIG       0x01    /* bank 0 */
#define W83795_REG_VOLT_CTRL1   0x02    /* bank 0 */
#define W83795_REG_VOLT_CTRL2   0x03    /* bank 0 */
#define W83795_REG_TEMP_CTRL1   0x04    /* bank 0 */
#define W83795_REG_TEMP_CTRL2   0x05    /* bank 0 */
#define W83795_REG_FANIN_CTRL1  0x06    /* bank 0 */
#define W83795_REG_FANIN_CTRL2  0x07    /* bank 0 */
#define W83795_REG_VRLSB        0x3C    /* bank 0: shared measurement LSBs */
#define W83795_REG_I2C_ADDR     0xFC
#define W83795_REG_DEVICEID     0xFB
#define W83795_REG_VENDORID     0xFD
#define W83795_REG_CHIPID       0xFE
#define W83795_REG_DEVICEID_A   0xFF

/* bank 3 (0x3xx) */
#define W83795_REG_DTSC         0x01
#define W83795_REG_DTSE         0x02

/* bank-0 measurement register windows */
#define W83795_VOLT_FIRST       0x10    /* in0 .. in20  (0x10 + index)   */
#define W83795_VOLT_LAST        0x24
#define W83795_DTS_FIRST        0x26    /* dts0 .. dts7 (0x26 + index)   */
#define W83795_DTS_LAST         0x2D
#define W83795_FAN_FIRST        0x2E    /* fan0 .. fan13 (0x2E + index)  */
#define W83795_FAN_LAST         0x3B

#define W83795_NUM_BANKS        4
#define W83795_BANK_SIZE        256

#define TYPE_W83795 "w83795"
OBJECT_DECLARE_SIMPLE_TYPE(W83795State, W83795)

struct W83795State {
    I2CSlave parent_obj;

    uint8_t bank;                                   /* last BANKSEL byte */
    uint8_t ptr;                                    /* register pointer  */
    uint32_t count;                                 /* bytes since START */
    uint8_t vrlsb;                                  /* shadow LSB (0x3C)  */

    /* generic backing store (limits, pwm duty, alarms, scratch) */
    uint8_t regs[W83795_NUM_BANKS][W83795_BANK_SIZE];
    /* bank-0 measurement high bytes and their associated VRLSB latch */
    uint8_t meas0[W83795_BANK_SIZE];
    uint8_t lsb0[W83795_BANK_SIZE];
};

static bool w83795_is_measurement(uint8_t reg)
{
    return (reg >= W83795_VOLT_FIRST && reg <= W83795_VOLT_LAST) ||
           (reg >= W83795_DTS_FIRST && reg <= W83795_DTS_LAST) ||
           (reg >= W83795_FAN_FIRST && reg <= W83795_FAN_LAST);
}

/* target voltage (mV) at "scale" mV/bit -> 10-bit value split high8 + VRLSB[7:6] */
static void w83795_set_volt(W83795State *s, uint8_t reg, unsigned mv,
                            unsigned scale)
{
    unsigned val = mv / scale;
    if (val > 0x3ff) {
        val = 0x3ff;
    }
    s->meas0[reg] = val >> 2;
    s->lsb0[reg] = (val & 0x3) << 6;
}

/* target fan speed (RPM) -> 12-bit tach count split high8 + VRLSB[7:4] */
static void w83795_set_fan(W83795State *s, uint8_t reg, unsigned rpm)
{
    unsigned cnt = rpm ? 1350000u / rpm : 0xfff;
    if (cnt == 0 || cnt > 0xffe) {
        cnt = 0xfff;                                /* stopped / unpopulated */
    }
    s->meas0[reg] = (cnt >> 4) & 0xff;
    s->lsb0[reg] = (cnt & 0xf) << 4;
}

/* target temperature (milli-degC) -> s8 whole degrees + VRLSB[7:6] quarters */
static void w83795_set_temp(W83795State *s, uint8_t reg, int mdeg)
{
    int whole = mdeg / 1000;
    int quarters = ((mdeg % 1000) + 125) / 250;     /* round to 0.25 degC */
    if (quarters > 3) {
        whole++;
        quarters -= 4;
    }
    s->meas0[reg] = (uint8_t)(int8_t)whole;
    s->lsb0[reg] = (quarters & 0x3) << 6;
}

static void w83795_load_defaults(W83795State *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->meas0, 0, sizeof(s->meas0));
    memset(s->lsb0, 0, sizeof(s->lsb0));
    s->bank = 0;
    s->ptr = 0;
    s->count = 0;
    s->vrlsb = 0;

    /*
     * Voltage rails (index = reg - 0x10). Indices 12..14 (regs 0x1C..0x1E) are
     * the 3VDD / 3VSB / VBAT rails and use 6 mV/bit; the rest use 2 mV/bit.
     * has_in from volt_ctrl 0xff/0xf7 covers in0..in10, in12..in15; the temp
     * control adds in16.  in11 (0x1B) is intentionally left unpopulated.
     */
    w83795_set_volt(s, 0x10, 1000, 2);   /* in0  VCORE CPU0  ~1.00 V */
    w83795_set_volt(s, 0x11, 1000, 2);   /* in1  VCORE CPU1  ~1.00 V */
    w83795_set_volt(s, 0x12, 1500, 2);   /* in2  VDIMM       ~1.50 V */
    w83795_set_volt(s, 0x13, 1100, 2);   /* in3  VTT/rail    ~1.10 V */
    w83795_set_volt(s, 0x14, 1800, 2);   /* in4              ~1.80 V */
    w83795_set_volt(s, 0x15, 1200, 2);   /* in5              ~1.20 V */
    w83795_set_volt(s, 0x16, 1050, 2);   /* in6              ~1.05 V */
    w83795_set_volt(s, 0x17, 1500, 2);   /* in7              ~1.50 V */
    w83795_set_volt(s, 0x18,  900, 2);   /* in8              ~0.90 V */
    w83795_set_volt(s, 0x19, 1250, 2);   /* in9              ~1.25 V */
    w83795_set_volt(s, 0x1A, 1350, 2);   /* in10             ~1.35 V */
    /* 0x1B (in11) unpopulated */
    w83795_set_volt(s, 0x1C, 3300, 6);   /* in12 3VDD  (3.3 V, 6 mV/bit) */
    w83795_set_volt(s, 0x1D, 3300, 6);   /* in13 3VSB  (3.3 V, 6 mV/bit) */
    w83795_set_volt(s, 0x1E, 3040, 6);   /* in14 VBAT  (~3.0 V, 6 mV/bit) */
    w83795_set_volt(s, 0x1F, 1520, 2);   /* in15             ~1.52 V */
    w83795_set_volt(s, 0x20, 1620, 2);   /* in16             ~1.62 V */

    /* CPU thermal diode (temp0, reg 0x21 == W83795_REG_TEMP[0][TEMP_READ]) */
    w83795_set_temp(s, 0x21, 42250);     /* 42.25 degC */

    /* DTS die temps (AMD SB-TSI), per socket */
    w83795_set_temp(s, 0x26, 45000);     /* dts0  CPU0 die 45 degC */
    w83795_set_temp(s, 0x27, 47000);     /* dts1  CPU1 die 47 degC */

    /* Fan tach (fan0..fan7 == fan1..fan8): a populated 6-fan chassis */
    w83795_set_fan(s, 0x2E, 4950);       /* fan1 */
    w83795_set_fan(s, 0x2F, 5100);       /* fan2 */
    w83795_set_fan(s, 0x30, 4800);       /* fan3 */
    w83795_set_fan(s, 0x31, 3600);       /* fan4 */
    w83795_set_fan(s, 0x32, 3750);       /* fan5 */
    w83795_set_fan(s, 0x33, 3900);       /* fan6 */
    w83795_set_fan(s, 0x34, 0);          /* fan7 unpopulated */
    w83795_set_fan(s, 0x35, 0);          /* fan8 unpopulated */

    /* PWM output duty (bank 2, regs 0x10..0x17): 100% like coreboot default */
    for (int i = 0; i < 8; i++) {
        s->regs[2][0x10 + i] = 0xff;
    }
}

static uint8_t w83795_do_read(W83795State *s, uint8_t bank, uint8_t reg)
{
    /* Bank-independent identification + bank-select registers. */
    switch (reg) {
    case W83795_REG_BANKSEL:
        return s->bank;
    case W83795_REG_VENDORID:
        return (s->bank & 0x80) ? 0x5c : 0xa3;
    case W83795_REG_CHIPID:
        return 0x79;
    case W83795_REG_DEVICEID:
        return 0x50;                                /* rev. A */
    case W83795_REG_DEVICEID_A:
        return 0x50;
    case W83795_REG_I2C_ADDR:
        return 0x2f;
    default:
        break;
    }

    if (bank == 0) {
        switch (reg) {
        case W83795_REG_CONFIG:
            return 0x01;                            /* started, CONFIG48=0 -> w83795g */
        case W83795_REG_VOLT_CTRL1:
            return 0xff;
        case W83795_REG_VOLT_CTRL2:
            return 0xf7;
        case W83795_REG_TEMP_CTRL1:
            return 0x2a;                            /* DTS enable + chan4/5 = voltage */
        case W83795_REG_TEMP_CTRL2:
            return 0x01;                            /* temp0 = thermal diode */
        case W83795_REG_FANIN_CTRL1:
            return 0xff;                            /* fan1..8 present */
        case W83795_REG_FANIN_CTRL2:
            return 0x00;
        case W83795_REG_VRLSB:
            return s->vrlsb;
        default:
            break;
        }
        if (w83795_is_measurement(reg)) {
            s->vrlsb = s->lsb0[reg];                /* latch the low bits */
            return s->meas0[reg];
        }
    }

    if (bank == 3) {
        switch (reg) {
        case W83795_REG_DTSC:
            return 0x01;                            /* DTS clock/source valid */
        case W83795_REG_DTSE:
            return 0x03;                            /* 2 DTS sources enabled */
        default:
            break;
        }
    }

    return s->regs[bank][reg];
}

static void w83795_do_write(W83795State *s, uint8_t bank, uint8_t reg,
                            uint8_t data)
{
    if (reg == W83795_REG_BANKSEL) {
        s->bank = data;                             /* low 3 bits select bank */
        return;
    }
    /* Everything else lands in the scratch store (limits, pwm, config). */
    s->regs[bank][reg] = data;
}

static uint8_t w83795_rx(I2CSlave *i2c)
{
    W83795State *s = W83795(i2c);
    uint8_t val = w83795_do_read(s, s->bank & 0x07, s->ptr);
    s->ptr++;                                       /* auto-increment */
    return val;
}

static int w83795_tx(I2CSlave *i2c, uint8_t data)
{
    W83795State *s = W83795(i2c);

    if (s->count == 0) {
        s->ptr = data;                              /* first byte = reg pointer */
    } else {
        w83795_do_write(s, s->bank & 0x07, s->ptr, data);
        s->ptr++;                                   /* auto-increment */
    }
    s->count++;
    return 0;
}

static int w83795_event(I2CSlave *i2c, enum i2c_event event)
{
    W83795State *s = W83795(i2c);

    if (event == I2C_START_SEND || event == I2C_START_RECV) {
        s->count = 0;
    }
    return 0;
}

static void w83795_realize(DeviceState *dev, Error **errp)
{
    W83795State *s = W83795(dev);

    w83795_load_defaults(s);
}

static const VMStateDescription vmstate_w83795 = {
    .name = "w83795",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(bank, W83795State),
        VMSTATE_UINT8(ptr, W83795State),
        VMSTATE_UINT32(count, W83795State),
        VMSTATE_UINT8(vrlsb, W83795State),
        VMSTATE_UINT8_2DARRAY(regs, W83795State,
                              W83795_NUM_BANKS, W83795_BANK_SIZE),
        VMSTATE_UINT8_ARRAY(meas0, W83795State, W83795_BANK_SIZE),
        VMSTATE_UINT8_ARRAY(lsb0, W83795State, W83795_BANK_SIZE),
        VMSTATE_I2C_SLAVE(parent_obj, W83795State),
        VMSTATE_END_OF_LIST()
    }
};

static void w83795_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->realize = w83795_realize;
    k->event = w83795_event;
    k->recv = w83795_rx;
    k->send = w83795_tx;
    dc->vmsd = &vmstate_w83795;
    dc->desc = "Nuvoton/Winbond W83795G hardware monitor";
}

static const TypeInfo w83795_info = {
    .name          = TYPE_W83795,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(W83795State),
    .class_init    = w83795_class_init,
};

static void w83795_register_types(void)
{
    type_register_static(&w83795_info);
}

type_init(w83795_register_types)
