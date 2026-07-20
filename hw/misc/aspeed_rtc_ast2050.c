/*
 * ASPEED AST2050 (G3) Real-Time Clock — counter style (datasheet §24).
 *
 * RTC00 counter (R: sec/min/hour/day), RTC08 reload, RTC0C control ([0] enable),
 * RTC10 restart (write 0x5A loads the counter from reload), RTC14 reset (write
 * 0x99 clears).
 *
 * The counter ADVANCES behaviourally (#158) while CONTROL[0] is set. The tick rate
 * is the RTC input clock / 32768, and the INPUT CLOCK is selected by SCU08[16]
 * (datasheet §24 "RTC clock source selection"); the model reads SCU08[16] so the
 * rate TRACKS the guest's choice, exactly like silicon:
 *   - SCU08[16]=0 (default): 32.768 kHz internal source -> /32768 -> 1 Hz = REAL
 *     TIME. Silicon-measured 1.00x over a clean 20 s window (evidence
 *     d14-zephyr/31-rtc-realtime-bit16-0-silicon.txt). The KGPE-D16 needs no
 *     EXTERNAL crystal (datasheet §2.19); the internal 32.768 kHz is present.
 *   - SCU08[16]=1: the 24 MHz "test only" tap -> /32768 -> 24e6/32768 = 732.42x.
 *     Zephyr's driver forces this (evidence d14-zephyr/14 measured ~732x).
 * See aspeed_rtc_ast2050_src_hz() below. CORRECTION: earlier this model (and
 * #158/#186) treated 732x as THE rate — that was a bit16=1 artifact; the fixed
 * Linux driver clears bit16, so it is a real-time clock. clk_hz (default 24 MHz)
 * supplies the test-tap frequency; clk_hz=0 forces a frozen RTC for tests.
 *
 * COUNTER BIT LAYOUT — a KNOWN CONFLICT (tracked as #186): datasheet §24 RTC00
 * defines FIELD-packed DayCnt[31:17]/HourCnt[16:12]/MinuCnt[11:6]/SecCnt[5:0],
 * but the silicon-VALIDATED Zephyr rtc_aspeed_g3 driver uses BYTE-packed
 * day[31:24]/hour[23:16]/min[15:8]/sec[7:0]. The one silicon set/get test cannot
 * distinguish them (sec 30->52 never wraps past 60, and sec is bits[5:0] in
 * both). This model advances BYTE-packed to match the firmware oracle — a
 * field-packed re-encode would corrupt the driver's values even without a wrap,
 * breaking the validated driver in QEMU. Resolving which layout the silicon
 * actually carries into needs a minute-wrap test on real hardware (#186).
 *
 * This code is licensed under the GPL version 2 or later.
 */
#include "qemu/osdep.h"
#include "hw/misc/aspeed_rtc_ast2050.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "exec/address-spaces.h"

#define RTC_COUNTER 0x00
#define RTC_RELOAD  0x08
#define RTC_CONTROL 0x0C
#define RTC_RESTART 0x10
#define RTC_RESET   0x14
#define RESTART_MAGIC 0x5Au
#define RESET_MAGIC   0x99u

#define RTC_ALARM   0x04            /* §24 RTC04: hour[16:12]/min[11:6]/sec[5:0] */
#define RTC_CTRL_ENABLE 0x1u        /* CONTROL[0] */
#define RTC_CTRL_ALARM_SEC  0x02u   /* RTC0C[1] enable second alarm */
#define RTC_CTRL_ALARM_MIN  0x04u   /* RTC0C[2] enable minute alarm */
#define RTC_CTRL_ALARM_HOUR 0x08u   /* RTC0C[3] enable hour alarm   */
#define RTC_CTRL_ALARM_DAY  0x10u   /* RTC0C[4] enable day alarm    */
#define RTC_CTRL_ALARM_MASK 0x1Eu   /* any of the four alarm enables */
#define RTC_TICK_DIV    32768u      /* datasheet §24: RTC input clock / 32768 */

/* Upper bound on the alarm catch-up scan window (RTC-seconds), a backstop for
 * pathological main-loop starvation. 200000 RTC-seconds is ~273 real seconds at
 * the 732x crystal-less rate — far beyond any realistic timer latency (normally
 * the scan is a single iteration). */
#define RTC_ALARM_SCAN_CAP  200000u

/* Byte-packed counter layout (matches the silicon-validated Zephyr driver; see
 * the #186 note in the file header). Decode a packed value to absolute seconds. */
static uint64_t rtc_unpack_seconds(uint32_t v)
{
    uint32_t sec  = (v >>  0) & 0xff;
    uint32_t min  = (v >>  8) & 0xff;
    uint32_t hour = (v >> 16) & 0xff;
    uint32_t day  = (v >> 24) & 0xff;
    return ((uint64_t)day * 86400u) + (hour * 3600u) + (min * 60u) + sec;
}

static uint32_t rtc_pack_seconds(uint64_t total)
{
    uint32_t sec  = total % 60; total /= 60;
    uint32_t min  = total % 60; total /= 60;
    uint32_t hour = total % 24; total /= 24;
    uint32_t day  = total & 0xff;   /* byte-packed day field is 8-bit */
    return (day << 24) | (hour << 16) | (min << 8) | sec;
}

/*
 * The RTC clock SOURCE is selected by SCU08[16] (datasheet §24 "RTC clock source
 * selection" + §2.19): 0 = the internal 32.768 kHz source (÷32768 -> 1 Hz = REAL
 * TIME, silicon-measured 1.00x, evidence d14-zephyr/31), 1 = the 24 MHz "test
 * only" tap (÷32768 -> 24e6/32768 = 732.42x). Read SCU08 from the SCU (mapped at
 * 0x1E6E2000) so the modelled tick rate TRACKS the guest's clock-source choice,
 * exactly like real hardware: a guest that leaves/selects the 32.768 kHz source
 * (the SoC default, and what the fixed Linux driver uses) gets a real-time RTC; a
 * guest that selects the 24 MHz test tap (Zephyr) gets the fast counter. clk_hz
 * (device property, default 24 MHz) supplies the test-tap frequency, and clk_hz==0
 * still forces an unclocked (frozen) RTC for tests. Previously the model always
 * used clk_hz (24 MHz -> always 732x), which HID the real-time-with-bit16=0
 * behaviour that silicon showed (#158/#186 correction).
 */
#define ASPEED_G3_SCU08_CLK_SEL 0x1E6E2008u
#define SCU08_RTC_CLK_24M       (1u << 16)
static uint32_t aspeed_rtc_ast2050_src_hz(AspeedRtcAST2050State *s)
{
    uint32_t scu08;

    if (s->clk_hz == 0) {
        return 0;                       /* property override: frozen RTC */
    }
    scu08 = address_space_ldl_le(&address_space_memory, ASPEED_G3_SCU08_CLK_SEL,
                                 MEMTXATTRS_UNSPECIFIED, NULL);
    /* bit16=1 -> 24 MHz test tap (clk_hz); bit16=0 -> 32.768 kHz -> real time. */
    return (scu08 & SCU08_RTC_CLK_24M) ? s->clk_hz : RTC_TICK_DIV;
}

/* The live counter value: the held value in regs[COUNTER] plus the elapsed
 * "RTC seconds" since base_ns, but only while the RTC is enabled (CONTROL[0]).
 * When disabled the counter holds its value (datasheet §24: "If the RTC is
 * disabled, the {Sec,Minu,Hour}Cnt will hold the value"). */
static uint32_t aspeed_rtc_ast2050_counter(AspeedRtcAST2050State *s)
{
    uint32_t base = s->regs[RTC_COUNTER >> 2];
    uint32_t hz = aspeed_rtc_ast2050_src_hz(s);
    int64_t elapsed_ns;
    uint64_t ns_per_tick, ticks;

    if (!(s->regs[RTC_CONTROL >> 2] & RTC_CTRL_ENABLE) || hz == 0) {
        return base;
    }
    elapsed_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->base_ns;
    if (elapsed_ns <= 0) {
        return base;
    }
    /* ns per RTC tick = 32768 / src_hz seconds. Divide (not multiply) to avoid
     * overflow at long uptimes; 24 MHz => 1365333 ns/tick => 732.42 ticks/s;
     * 32.768 kHz => 1e9 ns/tick => 1 tick/s (real time). */
    ns_per_tick = ((uint64_t)RTC_TICK_DIV * 1000000000ull) / hz;
    if (ns_per_tick == 0) {
        return base;
    }
    ticks = (uint64_t)elapsed_ns / ns_per_tick;
    if (ticks == 0) {
        return base;
    }
    return rtc_pack_seconds(rtc_unpack_seconds(base) + ticks);
}

/* ns between two counter ticks (0 if the RTC is unclocked). */
static uint64_t aspeed_rtc_ast2050_ns_per_tick(AspeedRtcAST2050State *s)
{
    uint32_t hz = aspeed_rtc_ast2050_src_hz(s);

    if (hz == 0) {
        return 0;
    }
    return ((uint64_t)RTC_TICK_DIV * 1000000000ull) / hz;
}

/* True when every ENABLED RTC04 field (sec[1]/min[2]/hour[3]) equals the
 * corresponding field of the candidate counter value `cnt`.
 *
 * IMPORTANT: RTC04 is FIELD-packed (datasheet §24: hour[16:12]/min[11:6]/
 * sec[5:0]), which was PROVEN on real silicon (2026-07-21, evidence
 * d14-zephyr/28: a byte-packed alarm-hour write read back 0 and never matched).
 * The COUNTER read path is byte-packed (sec[7:0]/min[15:8]/hour[23:16]), so we
 * compare field VALUES: extract each field from RTC04 field-packed and from `cnt`
 * byte-packed. This makes the model reproduce the silicon behaviour — a
 * byte-packed alarm driver now FAILS to match here too, instead of passing.
 * RTC04 has no day field (17 bits), so RTC_CTRL_ALARM_DAY is not matched. */
static bool aspeed_rtc_ast2050_alarm_match_value(uint32_t ctrl, uint32_t alarm,
                                                 uint32_t cnt)
{
    if (!(ctrl & RTC_CTRL_ENABLE) || !(ctrl & RTC_CTRL_ALARM_MASK)) {
        return false;
    }
    if ((ctrl & RTC_CTRL_ALARM_SEC) &&
        ((cnt & 0xff) != (alarm & 0x3f))) {              /* RTC04 sec[5:0] */
        return false;
    }
    if ((ctrl & RTC_CTRL_ALARM_MIN) &&
        (((cnt >> 8) & 0xff) != ((alarm >> 6) & 0x3f))) { /* RTC04 min[11:6] */
        return false;
    }
    if ((ctrl & RTC_CTRL_ALARM_HOUR) &&
        (((cnt >> 16) & 0xff) != ((alarm >> 12) & 0x1f))) { /* RTC04 hour[16:12] */
        return false;
    }
    return true;
}

/* (Re)arm or disarm the alarm-check timer after any register change that could
 * affect the alarm (RTC04, RTC0C, RESTART, RESET). */
static void aspeed_rtc_ast2050_alarm_update(AspeedRtcAST2050State *s)
{
    uint32_t ctrl = s->regs[RTC_CONTROL >> 2];
    uint64_t ns_per_tick = aspeed_rtc_ast2050_ns_per_tick(s);

    if ((ctrl & RTC_CTRL_ENABLE) && (ctrl & RTC_CTRL_ALARM_MASK) &&
        ns_per_tick != 0) {
        uint32_t cnt = aspeed_rtc_ast2050_counter(s);

        /* Anchor the catch-up scan at the current count (so we do not
         * retroactively fire for counts before the alarm was armed) and seed the
         * rising-edge state with whether the counter already matches now. */
        s->alarm_last_abs = rtc_unpack_seconds(cnt);
        s->alarm_matched = aspeed_rtc_ast2050_alarm_match_value(
            ctrl, s->regs[RTC_ALARM >> 2], cnt);
        timer_mod(s->alarm_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns_per_tick);
    } else {
        timer_del(s->alarm_timer);
        s->alarm_matched = false;
        qemu_irq_lower(s->irq);
    }
}

/*
 * Fires roughly once per RTC-second while armed. To match silicon — where the
 * alarm comparator asserts (and the VIC latches the edge) the instant the
 * counter reaches the alarm value, and cannot be starved by software — this
 * SCANS every counter value crossed since the previous check and pulses
 * alarm_irq on a rising match edge. A naive "compare only the current live
 * counter" sample would skip the one-tick-per-day match whenever this timer runs
 * late (e.g. a tight guest poll loop starving the QEMU main loop) and then never
 * fire until the next day. The scan is O(gap) — normally 1 iteration (the timer
 * fires ~on time), capped for pathological starvation.
 */
static void aspeed_rtc_ast2050_alarm_tick(void *opaque)
{
    AspeedRtcAST2050State *s = opaque;
    uint32_t ctrl = s->regs[RTC_CONTROL >> 2];
    uint32_t alarm = s->regs[RTC_ALARM >> 2];
    uint64_t ns_per_tick = aspeed_rtc_ast2050_ns_per_tick(s);
    uint64_t cur_abs = rtc_unpack_seconds(aspeed_rtc_ast2050_counter(s));
    uint64_t start = s->alarm_last_abs;
    bool prev = s->alarm_matched;
    bool rising = false;

    /* Cap the look-back window; any realistic timer latency is a handful of
     * ticks, far below this. */
    if (cur_abs > start + RTC_ALARM_SCAN_CAP) {
        start = cur_abs - RTC_ALARM_SCAN_CAP;
    }
    for (uint64_t a = start + 1; a <= cur_abs; a++) {
        bool m = aspeed_rtc_ast2050_alarm_match_value(ctrl, alarm,
                                                      rtc_pack_seconds(a));
        if (m && !prev) {
            rising = true;
        }
        prev = m;
    }
    if (rising) {
        qemu_irq_pulse(s->irq);   /* rising edge -> VIC latches it */
    }
    s->alarm_matched = prev;
    s->alarm_last_abs = cur_abs;

    if (ns_per_tick != 0 &&
        (ctrl & RTC_CTRL_ENABLE) && (ctrl & RTC_CTRL_ALARM_MASK)) {
        timer_mod(s->alarm_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns_per_tick);
    }
}

static uint64_t aspeed_rtc_ast2050_read(void *opaque, hwaddr offset, unsigned size)
{
    AspeedRtcAST2050State *s = ASPEED_RTC_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_RTC_AST2050_NR_REGS) {
        return 0;
    }
    if (offset == RTC_COUNTER) {
        return aspeed_rtc_ast2050_counter(s);
    }
    return s->regs[reg];
}

static void aspeed_rtc_ast2050_write(void *opaque, hwaddr offset, uint64_t data,
                                     unsigned size)
{
    AspeedRtcAST2050State *s = ASPEED_RTC_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_RTC_AST2050_NR_REGS) {
        return;
    }
    switch (offset) {
    case RTC_COUNTER:
        /* counter status is read-only */
        break;
    case RTC_CONTROL: {
        uint32_t old = s->regs[RTC_CONTROL >> 2];
        if ((old ^ data) & RTC_CTRL_ENABLE) {
            if (data & RTC_CTRL_ENABLE) {
                /* enabling: start counting from the held value, now */
                s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            } else {
                /* disabling: freeze the advanced value into COUNTER */
                s->regs[RTC_COUNTER >> 2] = aspeed_rtc_ast2050_counter(s);
            }
        }
        s->regs[RTC_CONTROL >> 2] = data;
        break;
    }
    case RTC_RESTART:
        /* magic 0x5A loads the counter from the reload register and re-anchors
         * the count so it advances up from the freshly loaded value */
        if (data == RESTART_MAGIC) {
            s->regs[RTC_COUNTER >> 2] = s->regs[RTC_RELOAD >> 2];
            s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        break;
    case RTC_RESET:
        if (data == RESET_MAGIC) {
            memset(s->regs, 0, sizeof(s->regs));
            s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        break;
    default:
        s->regs[reg] = data;
        break;
    }

    /* Any write to RTC04/RTC0C/RESTART/RESET can arm/disarm or move the alarm. */
    aspeed_rtc_ast2050_alarm_update(s);
}

static const MemoryRegionOps aspeed_rtc_ast2050_ops = {
    .read = aspeed_rtc_ast2050_read,
    .write = aspeed_rtc_ast2050_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void aspeed_rtc_ast2050_reset(DeviceState *dev)
{
    AspeedRtcAST2050State *s = ASPEED_RTC_AST2050(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_del(s->alarm_timer);
    s->alarm_matched = false;
    s->alarm_last_abs = 0;
    qemu_irq_lower(s->irq);
}

static void aspeed_rtc_ast2050_realize(DeviceState *dev, Error **errp)
{
    AspeedRtcAST2050State *s = ASPEED_RTC_AST2050(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &aspeed_rtc_ast2050_ops, s,
                          TYPE_ASPEED_RTC_AST2050, 0x20);
    sysbus_init_mmio(sbd, &s->iomem);
    /*
     * The G3 RTC has a SINGLE interrupt line = VIC source 22 (silicon-proven,
     * #192/evidence d14-zephyr/28): the alarm fires on VIC 22, NOT a separate
     * source 26 as previously assumed. So there is one sysbus IRQ (index 0), and
     * the alarm pulses it. (RTC0C has only alarm-enables [1:4], no periodic-
     * interrupt-enable, so the RTC's sole interrupt is the alarm.)
     */
    sysbus_init_irq(sbd, &s->irq);         /* index 0: RTC IRQ = alarm (VIC 22) */
    s->alarm_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  aspeed_rtc_ast2050_alarm_tick, s);
}

static const VMStateDescription vmstate_aspeed_rtc_ast2050 = {
    .name = TYPE_ASPEED_RTC_AST2050,
    .version_id = 4,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, AspeedRtcAST2050State,
                             ASPEED_RTC_AST2050_NR_REGS),
        VMSTATE_INT64_V(base_ns, AspeedRtcAST2050State, 2),
        VMSTATE_TIMER_PTR_V(alarm_timer, AspeedRtcAST2050State, 3),
        VMSTATE_BOOL_V(alarm_matched, AspeedRtcAST2050State, 3),
        VMSTATE_UINT64_V(alarm_last_abs, AspeedRtcAST2050State, 4),
        VMSTATE_END_OF_LIST()
    }
};

static const Property aspeed_rtc_ast2050_properties[] = {
    /* RTC input-clock Hz; the /32768 tick divider makes the "second" rate.
     * KGPE-D16 default: 24 MHz (SCU08[16]=1, no 32.768 kHz crystal) => 732.42x. */
    DEFINE_PROP_UINT32("clk-hz", AspeedRtcAST2050State, clk_hz, 24000000),
};

static void aspeed_rtc_ast2050_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = aspeed_rtc_ast2050_realize;
    device_class_set_legacy_reset(dc, aspeed_rtc_ast2050_reset);
    dc->desc = "ASPEED AST2050 RTC (counter-style)";
    dc->vmsd = &vmstate_aspeed_rtc_ast2050;
    device_class_set_props(dc, aspeed_rtc_ast2050_properties);
}

static const TypeInfo aspeed_rtc_ast2050_info = {
    .name = TYPE_ASPEED_RTC_AST2050,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AspeedRtcAST2050State),
    .class_init = aspeed_rtc_ast2050_class_init,
};

static void aspeed_rtc_ast2050_register_types(void)
{
    type_register_static(&aspeed_rtc_ast2050_info);
}

type_init(aspeed_rtc_ast2050_register_types)
