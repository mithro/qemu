/*
 * ASPEED AST2050 (G3) Real-Time Clock — counter style.
 *
 * Datasheet §24: RTC00 counter status (R: [5:0]sec [11:6]min [16:12]hour
 * [31:17]day), RTC08 reload, RTC0C control ([0]enable), RTC10 restart (write 0x5A
 * to load the counter from reload), RTC14 reset (write 0x99). Four independent
 * up-counters; SecCnt ticks at 1 Hz off CLK32K. NOT the AST2400 BCD/CMOS RTC.
 * See qemu-model/peripherals/rtc.
 *
 * This code is licensed under the GPL version 2 or later.
 */
#ifndef ASPEED_RTC_AST2050_H
#define ASPEED_RTC_AST2050_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_ASPEED_RTC_AST2050 "aspeed.rtc-ast2050"
OBJECT_DECLARE_SIMPLE_TYPE(AspeedRtcAST2050State, ASPEED_RTC_AST2050)

#define ASPEED_RTC_AST2050_NR_REGS (0x20 / 4)

struct AspeedRtcAST2050State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[ASPEED_RTC_AST2050_NR_REGS];
};

#endif /* ASPEED_RTC_AST2050_H */
