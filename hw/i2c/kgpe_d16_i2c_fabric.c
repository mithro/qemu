/*
 * ASUS KGPE-D16 I2C mux fabric (QU9 + QU5 + U23) behind BMC bus I2C2.
 *
 * Faithful model of the board-level switching fabric between the AST2050's
 * I2C2 engine and the DIMM SPD/TSOD buses, decoded pin-by-pin from the
 * board's .FZ netlist (see
 * asus-kgpe-d16-firmware/schematic-wiring/I2C-MUX-FABRIC-ARBITRATION.md):
 *
 *  - QU9 (SN74CBTLV3125 FET switch): bridges I2C2 onto the QU5 common
 *    (net I2C7) ONLY while the board's SYS_PWRGD is high — its OE# pins are
 *    hard-wired to inverted SYS_PWRGD (74LVC14A U8 13->12). The BMC has no
 *    GPIO control over this; with the host off nothing behind the fabric
 *    ACKs.
 *  - QU5 (74HC4052 dual 4-ch analog mux): E# is strapped to GND (always
 *    enabled); channel = S1:S0.  Y0 -> aux-panel/TPM/PCIe-SMBus segment,
 *    Y1 -> unconnected, Y2 -> I2C10 (DIMM A-D SPD/TSOD), Y3 -> I2C11
 *    (DIMM E-H SPD/TSOD).
 *  - U23 (74LVC125) select-source arbitration: S1:S0 are driven by the BMC's
 *    GPIOF5/GPIOF4 only while BMC_PRESENT# AND SB_BIOS_POST_COMPLT# are both
 *    low (hardware mutex via D27/QQ9/QQ10); otherwise the SP5100 owns them
 *    (e.g. while the host BIOS reads SPD during POST). Both select nets have
 *    4.7k pull-ups, so the unclaimed/idle state is S1:S0 = 11 (bank E-H).
 *
 * The two QU9 switch pairs (I2C2<->I2C7 and I2C8<->I2C13) share one enable,
 * so from the BMC's point of view the whole fabric is reachable iff
 * SYS_PWRGD is high; this model collapses both pairs into that single gate.
 * The SP5100 side of the fabric (as an I2C master) is not modeled - only its
 * *ownership* of the select lines is.
 *
 * Copyright 2026, Apache-2.0 OR GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/kgpe_d16_i2c_fabric.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

OBJECT_DECLARE_SIMPLE_TYPE(KgpeD16I2cFabricState, KGPE_D16_I2C_FABRIC)

#define FABRIC_NUM_CHANNELS 4

typedef struct KgpeD16I2cFabricState {
    I2CSlave parent_obj;

    I2CBus *bus[FABRIC_NUM_CHANNELS];   /* y0..y3 */

    /* BMC-driven QU5 selects (GPIOF4=S0, GPIOF5=S1); pull-ups default 1. */
    uint8_t bmc_sel;
    /* Board SYS_PWRGD level: gates QU9 (nothing reachable while low). */
    bool sys_pwrgd;
    /*
     * U23 ownership inputs. sb_post_complt_n models SB_BIOS_POST_COMPLT#
     * (high = host still in POST -> SP5100 owns the selects). The kgpe-d16
     * machine has no host-firmware timeline, so the board glue ties this to
     * !SYS_PWRGD (POST "completes" as soon as power is good); tests drive the
     * "sb-post-complt-n" line high to exercise the host-owned window.
     * bmc_present_n is BMC_PRESENT# - constant low on a board with the BMC
     * fitted (it is running this machine), settable only for completeness.
     */
    bool sb_post_complt_n;
    bool bmc_present_n;
    /* What the (unmodeled) SP5100 drives on S1:S0 while it owns them. */
    uint8_t sb_select;
} KgpeD16I2cFabricState;

static bool kgpe_d16_i2c_fabric_bmc_owns(KgpeD16I2cFabricState *s)
{
    return !s->bmc_present_n && !s->sb_post_complt_n;
}

static uint8_t kgpe_d16_i2c_fabric_channel(KgpeD16I2cFabricState *s)
{
    return kgpe_d16_i2c_fabric_bmc_owns(s) ? s->bmc_sel : (s->sb_select & 3);
}

/*
 * Transparent forwarding: the fabric itself never ACKs any address (a FET
 * switch and an analog mux are not I2C devices); it just exposes whichever
 * channel QU5 currently routes - and only while QU9 is closed.
 */
static bool kgpe_d16_i2c_fabric_match(I2CSlave *candidate, uint8_t address,
                                      bool broadcast,
                                      I2CNodeList *current_devs)
{
    KgpeD16I2cFabricState *s = KGPE_D16_I2C_FABRIC(candidate);

    if (!s->sys_pwrgd) {
        /* QU9 open: the whole fabric is electrically disconnected. */
        return broadcast;
    }

    if (i2c_scan_bus(s->bus[kgpe_d16_i2c_fabric_channel(s)], address,
                     broadcast, current_devs)) {
        return true;
    }

    return broadcast;
}

static void kgpe_d16_i2c_fabric_select(void *opaque, int line, int level)
{
    KgpeD16I2cFabricState *s = KGPE_D16_I2C_FABRIC(opaque);

    if (level) {
        s->bmc_sel |= 1 << line;
    } else {
        s->bmc_sel &= ~(1 << line);
    }
}

static void kgpe_d16_i2c_fabric_sys_pwrgd(void *opaque, int line, int level)
{
    KgpeD16I2cFabricState *s = KGPE_D16_I2C_FABRIC(opaque);

    s->sys_pwrgd = !!level;
    /*
     * Board-glue simplification (see the struct comment): POST completes as
     * soon as host power is good. A test that has explicitly raised
     * sb-post-complt-n keeps its setting only while power stays up.
     */
    s->sb_post_complt_n = !level;
}

static void kgpe_d16_i2c_fabric_sb_post(void *opaque, int line, int level)
{
    KgpeD16I2cFabricState *s = KGPE_D16_I2C_FABRIC(opaque);

    s->sb_post_complt_n = !!level;
}

static void kgpe_d16_i2c_fabric_reset_hold(Object *obj, ResetType type)
{
    KgpeD16I2cFabricState *s = KGPE_D16_I2C_FABRIC(obj);

    /* Select nets rest at the 4.7k pull-up state; host power is off. */
    s->bmc_sel = 3;
    s->sys_pwrgd = false;
    s->sb_post_complt_n = true;
}

static void kgpe_d16_i2c_fabric_init(Object *obj)
{
    KgpeD16I2cFabricState *s = KGPE_D16_I2C_FABRIC(obj);
    static const char *const names[FABRIC_NUM_CHANNELS] = {
        "y0-auxpanel", "y1-nc", "y2-dimm-ad", "y3-dimm-eh",
    };
    int i;

    for (i = 0; i < FABRIC_NUM_CHANNELS; i++) {
        s->bus[i] = i2c_init_bus(DEVICE(s), names[i]);
    }

    qdev_init_gpio_in_named(DEVICE(s), kgpe_d16_i2c_fabric_select,
                            "select", 2);
    qdev_init_gpio_in_named(DEVICE(s), kgpe_d16_i2c_fabric_sys_pwrgd,
                            "sys-pwrgd", 1);
    qdev_init_gpio_in_named(DEVICE(s), kgpe_d16_i2c_fabric_sb_post,
                            "sb-post-complt-n", 1);
}

I2CBus *kgpe_d16_i2c_fabric_get_bus(DeviceState *dev, uint8_t channel)
{
    KgpeD16I2cFabricState *s = KGPE_D16_I2C_FABRIC(dev);

    g_assert(channel < FABRIC_NUM_CHANNELS);
    return s->bus[channel];
}

static const VMStateDescription vmstate_kgpe_d16_i2c_fabric = {
    .name = TYPE_KGPE_D16_I2C_FABRIC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, KgpeD16I2cFabricState),
        VMSTATE_UINT8(bmc_sel, KgpeD16I2cFabricState),
        VMSTATE_BOOL(sys_pwrgd, KgpeD16I2cFabricState),
        VMSTATE_BOOL(sb_post_complt_n, KgpeD16I2cFabricState),
        VMSTATE_BOOL(bmc_present_n, KgpeD16I2cFabricState),
        VMSTATE_UINT8(sb_select, KgpeD16I2cFabricState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property kgpe_d16_i2c_fabric_props[] = {
    DEFINE_PROP_BOOL("bmc-present-n", KgpeD16I2cFabricState,
                     bmc_present_n, false),
    DEFINE_PROP_UINT8("sb-select", KgpeD16I2cFabricState, sb_select, 0),
};

static void kgpe_d16_i2c_fabric_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "KGPE-D16 QU9/QU5/U23 I2C mux fabric";
    dc->vmsd = &vmstate_kgpe_d16_i2c_fabric;
    device_class_set_props(dc, kgpe_d16_i2c_fabric_props);
    rc->phases.hold = kgpe_d16_i2c_fabric_reset_hold;
    sc->match_and_add = kgpe_d16_i2c_fabric_match;
}

static const TypeInfo kgpe_d16_i2c_fabric_info = {
    .name          = TYPE_KGPE_D16_I2C_FABRIC,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(KgpeD16I2cFabricState),
    .instance_init = kgpe_d16_i2c_fabric_init,
    .class_init    = kgpe_d16_i2c_fabric_class_init,
};

static void kgpe_d16_i2c_fabric_register_types(void)
{
    type_register_static(&kgpe_d16_i2c_fabric_info);
}

type_init(kgpe_d16_i2c_fabric_register_types)
