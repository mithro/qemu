/*
 * ASPEED Watchdog Controller
 *
 * Copyright (C) 2016-2017 IBM Corp.
 *
 * This code is licensed under the GPL version 2 or later. See the
 * COPYING file in the top-level directory.
 */

#ifndef WDT_ASPEED_H
#define WDT_ASPEED_H

#include "hw/misc/aspeed_scu.h"
#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_ASPEED_WDT "aspeed.wdt"
OBJECT_DECLARE_TYPE(AspeedWDTState, AspeedWDTClass, ASPEED_WDT)
#define TYPE_ASPEED_2400_WDT TYPE_ASPEED_WDT "-ast2400"
#define TYPE_ASPEED_2500_WDT TYPE_ASPEED_WDT "-ast2500"
#define TYPE_ASPEED_2600_WDT TYPE_ASPEED_WDT "-ast2600"
#define TYPE_ASPEED_2700_WDT TYPE_ASPEED_WDT "-ast2700"
#define TYPE_ASPEED_1030_WDT TYPE_ASPEED_WDT "-ast1030"

#define ASPEED_WDT_REGS_MAX        (0x80 / 4)

struct AspeedWDTState {
    /*< private >*/
    SysBusDevice parent_obj;
    QEMUTimer *timer;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[ASPEED_WDT_REGS_MAX];

    /*
     * Timeout interrupt (datasheet §27, WDT0C[2] "wdt_intr"): when the counter
     * reaches zero the WDT can raise this interrupt INSTEAD of resetting the SoC.
     * Left unconnected on machines that only use reset-mode (raising an
     * unconnected qemu_irq is a no-op), wired to the VIC on the AST2050 (G3).
     */
    qemu_irq irq;

    AspeedSCUState *scu;
    uint32_t pclk_freq;
};


struct AspeedWDTClass {
    SysBusDeviceClass parent_class;

    uint32_t iosize;
    uint32_t ext_pulse_width_mask;
    uint32_t reset_ctrl_reg;
    void (*reset_pulse)(AspeedWDTState *s, uint32_t property);
    void (*wdt_reload)(AspeedWDTState *s);
    uint64_t (*sanitize_ctrl)(uint64_t data);
    uint32_t default_status;
    uint32_t default_reload_value;
};

#endif /* WDT_ASPEED_H */
