/*
 * Generic PMBus power supply (PSU) telemetry device.
 *
 * Faithful-enough model of the server PSU's SMBus/PMBus management link as
 * reached by the ASUS KGPE-D16 BMC on its I2C1 engine (schematic
 * asus-kgpe-d16-firmware/schematic-wiring/AST2050-BMC-WIRING.md §10.2 and
 * I2C-SMBUS-TOPOLOGY.md §3.1: connector PSUSMB1, BMC balls A15/B15 = SDA1/SCL1,
 * SMBALERT# on SALT1/B12).  The schematic lists the PSU device address as
 * "PSU-specific" (whatever supply is plugged into the chassis), so this models
 * a generic PMBus 1.2 compliant power supply at the conventional server-PSU
 * address 0x58, exposing the standard input / output / temperature / fan
 * telemetry and manufacturer identification that a BMC's pmbus hwmon reads.
 * It is deliberately a stand-in for the real PSU silicon, not a specific part.
 *
 * All register behaviour comes from the PMBus base class
 * (hw/i2c/pmbus_device.c).  This model only declares which telemetry the PSU
 * page supports (page flags) and seeds faithful reset/default readings, so it
 * is entirely register-driven with no device-specific command handling.
 *
 * Deterministic seeded readings (page 0):
 *   PMBUS_REVISION      0x98 -> 0x22   PMBus revision 1.2 (byte)
 *   PMBUS_CAPABILITY    0x19 -> 0x30   400 kHz max + SMBALERT#, PEC off (byte)
 *   READ_VIN            0x88 -> 0x00E6 LINEAR11, 230 V AC input
 *   READ_VOUT           0x8B -> 0x1800 ULINEAR16 (VOUT_MODE exp -9), 12.0 V
 *   READ_IOUT           0x8C -> 0x0008 LINEAR11, 8 A
 *   READ_TEMPERATURE_1  0x8D -> 0x001E LINEAR11, 30 C
 *   READ_FAN_SPEED_1    0x90 -> 0x13E8 LINEAR11 (exp 2), 4000 RPM
 *   READ_PIN            0x97 -> 0x006E LINEAR11, 110 W
 *   READ_POUT           0x96 -> 0x0060 LINEAR11, 96 W
 *   STATUS_WORD         0x79 -> 0x0000 no faults, POWER_GOOD asserted
 *   MFR_ID              0x99 -> "QEMU"
 *   MFR_MODEL           0x9A -> "KGPE-D16-PSU"
 *
 * Simplification: the readings are fixed nominal values.  A real PSU only
 * brings its main rails up once the host is powered (the standby MCU stays
 * alive on +5VSB); gating READ_VOUT / POWER_GOOD on the modeled host-power
 * GPIO (as hw/i2c/kgpe_d16_i2c_fabric.c does with "sys-pwrgd") would be a
 * faithful future refinement.  Fixed values keep the smoke test deterministic.
 *
 * Copyright 2026, Apache-2.0 OR GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/pmbus_device.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define TYPE_PMBUS_PSU "pmbus-psu"
#define PMBUS_PSU(obj) OBJECT_CHECK(PMBusPsuState, (obj), TYPE_PMBUS_PSU)

typedef struct PMBusPsuState {
    PMBusDevice parent;
} PMBusPsuState;

/* PMBus identification / capability defaults */
#define PSU_PMBUS_REVISION      0x22        /* PMBus revision 1.2 */
#define PSU_CAPABILITY          0x30        /* 400 kHz max + SMBALERT#, no PEC */
#define PSU_VOUT_MODE           0x17        /* linear mode, 5-bit exp = -9 */
#define PSU_VOUT_EXP            (-9)

/* Faithful nominal telemetry (host running, +12 V main rail up) */
#define PSU_VIN_VOLTS           230         /* AC mains (AU/EU rig) */
#define PSU_IIN_AMPS            1
#define PSU_VOUT_VOLTS          12
#define PSU_IOUT_AMPS           8
#define PSU_POUT_WATTS          96
#define PSU_PIN_WATTS           110
#define PSU_TEMP_CELSIUS        30
#define PSU_FAN_RPM             4000
#define PSU_FAN_EXP             2

#define PSU_MFR_ID              "QEMU"
#define PSU_MFR_MODEL           "KGPE-D16-PSU"
#define PSU_MFR_REVISION        "1.0"

/*
 * Build a LINEAR11 word: a 5-bit signed exponent in bits [15:11] and an 11-bit
 * signed mantissa in bits [10:0].  pmbus_data2linear_mode() scales the value to
 * the mantissa; we OR in the exponent bits so a reader recovers value = mant *
 * 2^exp.  For exp 0 this is just the raw integer.
 */
static uint16_t psu_linear11(uint16_t value, int exp)
{
    return ((exp & 0x1F) << 11) | (pmbus_data2linear_mode(value, exp) & 0x7FF);
}

static void pmbus_psu_exit_reset(Object *obj, ResetType type)
{
    PMBusDevice *pmdev = PMBUS_DEVICE(obj);
    PMBusPage *page = &pmdev->pages[0];

    pmdev->page = 0;
    pmdev->capability = PSU_CAPABILITY;

    page->operation = PB_OP_ON;
    page->vout_mode = PSU_VOUT_MODE;
    page->revision = PSU_PMBUS_REVISION;

    page->read_vin = psu_linear11(PSU_VIN_VOLTS, 0);
    page->read_iin = psu_linear11(PSU_IIN_AMPS, 0);
    /* READ_VOUT is ULINEAR16: mantissa only, exponent lives in VOUT_MODE */
    page->read_vout = pmbus_data2linear_mode(PSU_VOUT_VOLTS, PSU_VOUT_EXP);
    page->read_iout = psu_linear11(PSU_IOUT_AMPS, 0);
    page->read_pout = psu_linear11(PSU_POUT_WATTS, 0);
    page->read_pin = psu_linear11(PSU_PIN_WATTS, 0);
    page->read_temperature_1 = psu_linear11(PSU_TEMP_CELSIUS, 0);
    page->read_fan_speed_1 = psu_linear11(PSU_FAN_RPM, PSU_FAN_EXP);

    /* Healthy: no faults, POWER_GOOD asserted (PB_STATUS_POWER_GOOD_N clear) */
    page->status_word = 0;

    page->mfr_id = PSU_MFR_ID;
    page->mfr_model = PSU_MFR_MODEL;
    page->mfr_revision = PSU_MFR_REVISION;
}

static void pmbus_psu_init(Object *obj)
{
    PMBusDevice *pmdev = PMBUS_DEVICE(obj);
    uint64_t flags = PB_HAS_VOUT_MODE | PB_HAS_VIN | PB_HAS_VOUT |
                     PB_HAS_IIN | PB_HAS_IOUT | PB_HAS_PIN | PB_HAS_POUT |
                     PB_HAS_TEMPERATURE | PB_HAS_FAN | PB_HAS_MFR_INFO;

    pmbus_page_config(pmdev, 0, flags);
}

static const VMStateDescription vmstate_pmbus_psu = {
    .name = TYPE_PMBUS_PSU,
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (const VMStateField[]){
        VMSTATE_PMBUS_DEVICE(parent, PMBusPsuState),
        VMSTATE_END_OF_LIST()
    }
};

static void pmbus_psu_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    PMBusDeviceClass *k = PMBUS_DEVICE_CLASS(klass);

    dc->desc = "Generic PMBus power supply (KGPE-D16 PSUSMB1)";
    dc->vmsd = &vmstate_pmbus_psu;
    k->device_num_pages = 1;
    rc->phases.exit = pmbus_psu_exit_reset;
}

static const TypeInfo pmbus_psu_info = {
    .name = TYPE_PMBUS_PSU,
    .parent = TYPE_PMBUS_DEVICE,
    .instance_size = sizeof(PMBusPsuState),
    .instance_init = pmbus_psu_init,
    .class_init = pmbus_psu_class_init,
};

static void pmbus_psu_register_types(void)
{
    type_register_static(&pmbus_psu_info);
}

type_init(pmbus_psu_register_types)
