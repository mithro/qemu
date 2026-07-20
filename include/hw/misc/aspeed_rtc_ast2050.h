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

    /*
     * Behavioural counter advance (#158). base_ns is the QEMU_CLOCK_VIRTUAL
     * timestamp from which the live count is measured relative to the value
     * currently held in regs[COUNTER]; it is re-anchored on a RESTART load and
     * on an enable transition. clk_hz is the RTC input-clock frequency: on the
     * crystal-less KGPE-D16 the firmware selects the 24 MHz source via
     * SCU08[16]=1 (datasheet §2.19 / §24: no external 32.768 kHz oscillator), so
     * the RTC's fixed /32768 tick divider yields 24e6/32768 = 732.42 "RTC
     * seconds" per real second — the fast rate measured on silicon.
     */
    int64_t base_ns;
    uint32_t clk_hz;
};

#endif /* ASPEED_RTC_AST2050_H */
