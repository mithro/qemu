/*
 * Digi NS9360 System Registers, Memory Controller, and BBus Stubs
 *
 * This device model provides three MMIO regions covering the NS9360's
 * system control, memory controller, and BBus peripheral bridge register
 * spaces.  Most registers are simple read/write storage to satisfy
 * firmware probing.  Special cases:
 *
 *   - SYS_BASE + 0x0188: PLL configuration register.
 *     Always returns 0x00CB0000 which encodes ND=11 (bits 16-20) and
 *     FS=1 (bits 21-22), giving a CPU clock of ~176 MHz from a
 *     29.4912 MHz crystal.
 *
 *   - SYS_BASE + 0x0044: Timer 0 reload/value (free-running countdown)
 *   - SYS_BASE + 0x0084: Timer 0 read (current count)
 *   - SYS_BASE + 0x0190: Timer control / timestamp
 *     Timer 0 is a free-running 32-bit countdown timer clocked from
 *     the AHB bus clock.  We model it using the QEMU virtual clock.
 *
 * Memory map:
 *   Region 0: System control registers  (0xA0900000, 256 KB)
 *   Region 1: Memory controller regs    (0xA0700000, 256 KB)
 *   Region 2: BBus bridge registers     (0x90600000, 256 KB)
 *
 * Copyright (c) 2025 The QEMU contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_NS9360_SYSREGS "ns9360-sysregs"
OBJECT_DECLARE_SIMPLE_TYPE(NS9360SysRegsState, NS9360_SYSREGS)

/* MMIO region sizes (full hardware address space) */
#define NS9360_SYS_SIZE     (256 * 1024)
#define NS9360_MEM_SIZE     (256 * 1024)
#define NS9360_BBUS_SIZE    (256 * 1024)

/*
 * Backing store for register values.
 *
 * We allocate 4 KB (1024 uint32_t registers) per region, which covers
 * offsets 0x000 through 0xFFF.  This is sufficient for all documented
 * NS9360 registers.  Accesses beyond this range return 0 on read and
 * are silently discarded on write.
 */
#define NS9360_REG_STORE_SIZE   (4 * 1024)
#define NS9360_NREGS            (NS9360_REG_STORE_SIZE / 4)  /* 1024 */

/* Special register offsets within the system control region */
#define NS9360_SYS_PLL_OFFSET       0x0188
#define NS9360_SYS_TIMER0_RELOAD    0x0044
#define NS9360_SYS_TIMER0_READ      0x0084
#define NS9360_SYS_TIMER_CTRL       0x0190

/*
 * PLL configuration value:
 *   Bits [20:16] = ND = 11  (0x0B)
 *   Bits [22:21] = FS = 1
 *   Encodes as: (11 << 16) | (1 << 21) = 0x002B0000
 *
 * However, the user specification states the value should be 0x00CB0000.
 * That corresponds to ND=11 with additional PLL enable/lock bits set:
 *   0x00CB0000 = (1 << 23) | (1 << 22) | (0x0B << 16)
 * We use the value as specified.
 */
#define NS9360_PLL_VALUE    0x00CB0000

/*
 * AHB clock frequency for the timer model.
 *
 * The NS9360 with ND=11, FS=1 and a 29.4912 MHz crystal produces
 * a CPU clock of approximately 176.9 MHz.  The AHB bus clock is
 * typically CPU/2 = ~88.5 MHz.  We use a round value that is close
 * enough for timer emulation purposes.
 */
#define NS9360_AHB_FREQ_HZ  88000000U

struct NS9360SysRegsState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion sys_iomem;
    MemoryRegion mem_iomem;
    MemoryRegion bbus_iomem;

    /*
     * Register storage arrays.
     * Indexed by (offset / 4), capped at NS9360_NREGS entries.
     */
    uint32_t sys_regs[NS9360_NREGS];
    uint32_t mem_regs[NS9360_NREGS];
    uint32_t bbus_regs[NS9360_NREGS];

    /* Timer 0: start time for free-running countdown */
    int64_t timer0_start_ns;
    uint32_t timer0_reload;
};

/* ------------------------------------------------------------------ */
/* Timer 0 helper                                                      */
/* ------------------------------------------------------------------ */

/*
 * Return the current value of the free-running countdown timer.
 * The timer counts down from timer0_reload (default 0xFFFFFFFF)
 * at the AHB clock rate.
 */
static uint32_t ns9360_timer0_get_count(NS9360SysRegsState *s)
{
    int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t elapsed_ns = now_ns - s->timer0_start_ns;
    uint64_t elapsed_ticks;

    if (elapsed_ns < 0) {
        elapsed_ns = 0;
    }

    /*
     * Convert elapsed nanoseconds to AHB clock ticks.
     * ticks = elapsed_ns * AHB_FREQ / 1e9
     *
     * Use muldiv64 to avoid overflow.
     */
    elapsed_ticks = muldiv64((uint64_t)elapsed_ns, NS9360_AHB_FREQ_HZ,
                             NANOSECONDS_PER_SECOND);

    /*
     * The timer wraps around at 32 bits.  Subtract from the reload
     * value to get a countdown.
     */
    return (uint32_t)(s->timer0_reload - (uint32_t)elapsed_ticks);
}

/* ------------------------------------------------------------------ */
/* System control register region (Region 0)                           */
/* ------------------------------------------------------------------ */

static uint64_t ns9360_sys_read(void *opaque, hwaddr offset, unsigned size)
{
    NS9360SysRegsState *s = opaque;
    uint32_t idx = offset / 4;

    switch (offset) {
    case NS9360_SYS_PLL_OFFSET:
        return NS9360_PLL_VALUE;

    case NS9360_SYS_TIMER0_READ:
        return ns9360_timer0_get_count(s);

    case NS9360_SYS_TIMER0_RELOAD:
        return s->timer0_reload;

    case NS9360_SYS_TIMER_CTRL:
        /* Return stored value (firmware may write enable bits) */
        if (idx < NS9360_NREGS) {
            return s->sys_regs[idx];
        }
        return 0;

    default:
        if (idx < NS9360_NREGS) {
            return s->sys_regs[idx];
        }
        return 0;
    }
}

static void ns9360_sys_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    NS9360SysRegsState *s = opaque;
    uint32_t idx = offset / 4;

    switch (offset) {
    case NS9360_SYS_TIMER0_RELOAD:
        s->timer0_reload = (uint32_t)value;
        /* Restart the timer from this new value */
        s->timer0_start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        break;

    default:
        if (idx < NS9360_NREGS) {
            s->sys_regs[idx] = (uint32_t)value;
        }
        break;
    }
}

static const MemoryRegionOps ns9360_sys_ops = {
    .read = ns9360_sys_read,
    .write = ns9360_sys_write,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* ------------------------------------------------------------------ */
/* Memory controller register region (Region 1)                        */
/* ------------------------------------------------------------------ */

static uint64_t ns9360_mem_read(void *opaque, hwaddr offset, unsigned size)
{
    NS9360SysRegsState *s = opaque;
    uint32_t idx = offset / 4;

    if (idx < NS9360_NREGS) {
        return s->mem_regs[idx];
    }
    return 0;
}

static void ns9360_mem_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    NS9360SysRegsState *s = opaque;
    uint32_t idx = offset / 4;

    if (idx < NS9360_NREGS) {
        s->mem_regs[idx] = (uint32_t)value;
    }
}

static const MemoryRegionOps ns9360_mem_ops = {
    .read = ns9360_mem_read,
    .write = ns9360_mem_write,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* ------------------------------------------------------------------ */
/* BBus bridge register region (Region 2)                              */
/* ------------------------------------------------------------------ */

static uint64_t ns9360_bbus_read(void *opaque, hwaddr offset, unsigned size)
{
    NS9360SysRegsState *s = opaque;
    uint32_t idx = offset / 4;

    if (idx < NS9360_NREGS) {
        return s->bbus_regs[idx];
    }
    return 0;
}

static void ns9360_bbus_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    NS9360SysRegsState *s = opaque;
    uint32_t idx = offset / 4;

    if (idx < NS9360_NREGS) {
        s->bbus_regs[idx] = (uint32_t)value;
    }
}

static const MemoryRegionOps ns9360_bbus_ops = {
    .read = ns9360_bbus_read,
    .write = ns9360_bbus_write,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* ------------------------------------------------------------------ */
/* Device lifecycle                                                    */
/* ------------------------------------------------------------------ */

static void ns9360_sysregs_reset(DeviceState *dev)
{
    NS9360SysRegsState *s = NS9360_SYSREGS(dev);

    memset(s->sys_regs, 0, sizeof(s->sys_regs));
    memset(s->mem_regs, 0, sizeof(s->mem_regs));
    memset(s->bbus_regs, 0, sizeof(s->bbus_regs));

    /*
     * Pre-load the PLL register so it is also visible via the
     * generic register array (in case firmware reads through
     * a non-special-case path).
     */
    s->sys_regs[NS9360_SYS_PLL_OFFSET / 4] = NS9360_PLL_VALUE;

    /* Timer 0: free-running countdown from 0xFFFFFFFF */
    s->timer0_reload = 0xFFFFFFFF;
    s->timer0_start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void ns9360_sysregs_init(Object *obj)
{
    NS9360SysRegsState *s = NS9360_SYSREGS(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    /* Region 0: System control registers */
    memory_region_init_io(&s->sys_iomem, obj, &ns9360_sys_ops, s,
                          "ns9360-sys", NS9360_SYS_SIZE);
    sysbus_init_mmio(sbd, &s->sys_iomem);

    /* Region 1: Memory controller registers */
    memory_region_init_io(&s->mem_iomem, obj, &ns9360_mem_ops, s,
                          "ns9360-memctrl", NS9360_MEM_SIZE);
    sysbus_init_mmio(sbd, &s->mem_iomem);

    /* Region 2: BBus bridge registers */
    memory_region_init_io(&s->bbus_iomem, obj, &ns9360_bbus_ops, s,
                          "ns9360-bbus", NS9360_BBUS_SIZE);
    sysbus_init_mmio(sbd, &s->bbus_iomem);
}

static const VMStateDescription vmstate_ns9360_sysregs = {
    .name = "ns9360-sysregs",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(sys_regs, NS9360SysRegsState, NS9360_NREGS),
        VMSTATE_UINT32_ARRAY(mem_regs, NS9360SysRegsState, NS9360_NREGS),
        VMSTATE_UINT32_ARRAY(bbus_regs, NS9360SysRegsState, NS9360_NREGS),
        VMSTATE_UINT32(timer0_reload, NS9360SysRegsState),
        VMSTATE_INT64(timer0_start_ns, NS9360SysRegsState),
        VMSTATE_END_OF_LIST()
    }
};

static void ns9360_sysregs_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, ns9360_sysregs_reset);
    dc->vmsd = &vmstate_ns9360_sysregs;
}

static const TypeInfo ns9360_sysregs_info = {
    .name          = TYPE_NS9360_SYSREGS,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NS9360SysRegsState),
    .instance_init = ns9360_sysregs_init,
    .class_init    = ns9360_sysregs_class_init,
};

static void ns9360_sysregs_register_types(void)
{
    type_register_static(&ns9360_sysregs_info);
}

type_init(ns9360_sysregs_register_types)
