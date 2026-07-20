/*
 * ASPEED AST2050 (G3) Real-Time Clock — counter style.
 *
 * Registers: RTC00 counter status (R), RTC08 reload, RTC0C control ([0]enable),
 * RTC10 restart (write 0x5A to load the counter from reload), RTC14 reset (write
 * 0x99). NOT the AST2400 BCD/CMOS RTC. See qemu-model/peripherals/rtc.
 *
 * COUNTER packing: the model advances the counter BYTE-packed
 * (sec[7:0]/min[15:8]/hour[23:16]/day[31:24]) to match the silicon-validated
 * Zephyr driver (drivers/rtc/rtc_aspeed_g3.c). NOTE: datasheet §24 documents a
 * FIELD-packed layout ([5:0]sec/[11:6]min/[16:12]hour/[31:17]day) — this conflict
 * is unresolved pending a silicon minute-wrap test and is tracked as #186; the
 * .c file header explains the choice.
 *
 * TICK RATE: the counter runs at clk_hz/32768 (datasheet §24 /32768 divider). On
 * the crystal-less KGPE-D16 clk_hz defaults to 24 MHz (SCU08[16]=1), so the
 * counter advances at 24e6/32768 = 732.42 "RTC seconds"/real second — NOT 1 Hz.
 * See the .c file for the full derivation and #158.
 *
 * This code is licensed under the GPL version 2 or later.
 */
#ifndef ASPEED_RTC_AST2050_H
#define ASPEED_RTC_AST2050_H

#include "hw/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_ASPEED_RTC_AST2050 "aspeed.rtc-ast2050"
OBJECT_DECLARE_SIMPLE_TYPE(AspeedRtcAST2050State, ASPEED_RTC_AST2050)

#define ASPEED_RTC_AST2050_NR_REGS (0x20 / 4)

struct AspeedRtcAST2050State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    /*
     * The G3 RTC has a SINGLE interrupt line = VIC source 22 (silicon-proven,
     * #192): the alarm (RTC04/RTC0C[1:4]) fires on it. There is NO separate
     * source-26 alarm IRQ — that was an incorrect assumption.
     */
    qemu_irq irq;        /* RTC IRQ = alarm (VIC 22) */

    uint32_t regs[ASPEED_RTC_AST2050_NR_REGS];

    /*
     * Alarm (#187): a periodic timer runs at the counter's own RTC-second rate
     * while the alarm is armed (RTC0C[0] enabled AND >=1 of RTC0C[1:4] set). To
     * match silicon — where the alarm comparator is combinational and the VIC
     * latches the match edge the instant the counter reaches the alarm value,
     * regardless of what software is doing — each tick does a CATCH-UP SCAN of
     * every counter value crossed since the previous check (alarm_last_abs) and
     * pulses the RTC irq (VIC 22) on a rising match edge. This is robust to the timer firing
     * late (e.g. a tight guest poll loop starving the QEMU main loop): a sampling
     * model that only compared the single live counter value would skip the
     * one-tick-per-day match and never fire. alarm_matched is the edge-detect
     * state; alarm_last_abs is the absolute-seconds value last scanned.
     */
    QEMUTimer *alarm_timer;
    bool alarm_matched;
    uint64_t alarm_last_abs;

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
