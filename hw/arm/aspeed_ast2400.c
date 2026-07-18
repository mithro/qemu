/*
 * ASPEED SoC family
 *
 * Andrew Jeffery <andrew@aj.id.au>
 * Jeremy Kerr <jk@ozlabs.org>
 *
 * Copyright 2016 IBM Corp.
 *
 * This code is licensed under the GPL version 2 or later.  See
 * the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/misc/unimp.h"
#include "hw/arm/aspeed_soc.h"
#include "hw/char/serial-mm.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "hw/i2c/aspeed_i2c.h"
#include "hw/irq.h"
#include "net/net.h"
#include "system/system.h"
#include "target/arm/cpu-qom.h"

#define ASPEED_SOC_IOMEM_SIZE       0x00200000

static const hwaddr aspeed_soc_ast2400_memmap[] = {
    [ASPEED_DEV_SPI_BOOT]  = 0x00000000,
    [ASPEED_DEV_IOMEM]  = 0x1E600000,
    [ASPEED_DEV_FMC]    = 0x1E620000,
    [ASPEED_DEV_SPI1]   = 0x1E630000,
    [ASPEED_DEV_EHCI1]  = 0x1E6A1000,
    [ASPEED_DEV_VIC]    = 0x1E6C0000,
    [ASPEED_DEV_SDMC]   = 0x1E6E0000,
    [ASPEED_DEV_SCU]    = 0x1E6E2000,
    [ASPEED_DEV_HACE]   = 0x1E6E3000,
    [ASPEED_DEV_XDMA]   = 0x1E6E7000,
    [ASPEED_DEV_VIDEO]  = 0x1E700000,
    [ASPEED_DEV_ADC]    = 0x1E6E9000,
    [ASPEED_DEV_SRAM]   = 0x1E720000,
    [ASPEED_DEV_SDHCI]  = 0x1E740000,
    [ASPEED_DEV_GPIO]   = 0x1E780000,
    [ASPEED_DEV_RTC]    = 0x1E781000,
    [ASPEED_DEV_TIMER1] = 0x1E782000,
    [ASPEED_DEV_WDT]    = 0x1E785000,
    [ASPEED_DEV_PWM]    = 0x1E786000,
    [ASPEED_DEV_LPC]    = 0x1E789000,
    [ASPEED_DEV_IBT]    = 0x1E789140,
    [ASPEED_DEV_I2C]    = 0x1E78A000,
    [ASPEED_DEV_PECI]   = 0x1E78B000,
    [ASPEED_DEV_ETH1]   = 0x1E660000,
    [ASPEED_DEV_ETH2]   = 0x1E680000,
    [ASPEED_DEV_UART1]  = 0x1E783000,
    [ASPEED_DEV_UART2]  = 0x1E78D000,
    [ASPEED_DEV_UART3]  = 0x1E78E000,
    [ASPEED_DEV_UART4]  = 0x1E78F000,
    [ASPEED_DEV_UART5]  = 0x1E784000,
    [ASPEED_DEV_VUART]  = 0x1E787000,
    [ASPEED_DEV_SDRAM]  = 0x40000000,
};

static const hwaddr aspeed_soc_ast2500_memmap[] = {
    [ASPEED_DEV_SPI_BOOT]  = 0x00000000,
    [ASPEED_DEV_IOMEM]  = 0x1E600000,
    [ASPEED_DEV_FMC]    = 0x1E620000,
    [ASPEED_DEV_SPI1]   = 0x1E630000,
    [ASPEED_DEV_SPI2]   = 0x1E631000,
    [ASPEED_DEV_EHCI1]  = 0x1E6A1000,
    [ASPEED_DEV_EHCI2]  = 0x1E6A3000,
    [ASPEED_DEV_VIC]    = 0x1E6C0000,
    [ASPEED_DEV_SDMC]   = 0x1E6E0000,
    [ASPEED_DEV_SCU]    = 0x1E6E2000,
    [ASPEED_DEV_HACE]   = 0x1E6E3000,
    [ASPEED_DEV_XDMA]   = 0x1E6E7000,
    [ASPEED_DEV_ADC]    = 0x1E6E9000,
    [ASPEED_DEV_VIDEO]  = 0x1E700000,
    [ASPEED_DEV_SRAM]   = 0x1E720000,
    [ASPEED_DEV_SDHCI]  = 0x1E740000,
    [ASPEED_DEV_GPIO]   = 0x1E780000,
    [ASPEED_DEV_RTC]    = 0x1E781000,
    [ASPEED_DEV_TIMER1] = 0x1E782000,
    [ASPEED_DEV_WDT]    = 0x1E785000,
    [ASPEED_DEV_PWM]    = 0x1E786000,
    [ASPEED_DEV_LPC]    = 0x1E789000,
    [ASPEED_DEV_IBT]    = 0x1E789140,
    [ASPEED_DEV_I2C]    = 0x1E78A000,
    [ASPEED_DEV_PECI]   = 0x1E78B000,
    [ASPEED_DEV_ETH1]   = 0x1E660000,
    [ASPEED_DEV_ETH2]   = 0x1E680000,
    [ASPEED_DEV_UART1]  = 0x1E783000,
    [ASPEED_DEV_UART2]  = 0x1E78D000,
    [ASPEED_DEV_UART3]  = 0x1E78E000,
    [ASPEED_DEV_UART4]  = 0x1E78F000,
    [ASPEED_DEV_UART5]  = 0x1E784000,
    [ASPEED_DEV_VUART]  = 0x1E787000,
    [ASPEED_DEV_SDRAM]  = 0x80000000,
};

static const int aspeed_soc_ast2400_irqmap[] = {
    [ASPEED_DEV_UART1]  = 9,
    [ASPEED_DEV_UART2]  = 32,
    [ASPEED_DEV_UART3]  = 33,
    [ASPEED_DEV_UART4]  = 34,
    [ASPEED_DEV_UART5]  = 10,
    [ASPEED_DEV_VUART]  = 8,
    [ASPEED_DEV_FMC]    = 19,
    [ASPEED_DEV_EHCI1]  = 5,
    [ASPEED_DEV_EHCI2]  = 13,
    [ASPEED_DEV_SDMC]   = 0,
    [ASPEED_DEV_SCU]    = 21,
    [ASPEED_DEV_ADC]    = 31,
    [ASPEED_DEV_GPIO]   = 20,
    [ASPEED_DEV_RTC]    = 22,
    [ASPEED_DEV_TIMER1] = 16,
    [ASPEED_DEV_TIMER2] = 17,
    [ASPEED_DEV_TIMER3] = 18,
    [ASPEED_DEV_TIMER4] = 35,
    [ASPEED_DEV_TIMER5] = 36,
    [ASPEED_DEV_TIMER6] = 37,
    [ASPEED_DEV_TIMER7] = 38,
    [ASPEED_DEV_TIMER8] = 39,
    [ASPEED_DEV_WDT]    = 27,
    [ASPEED_DEV_PWM]    = 28,
    [ASPEED_DEV_LPC]    = 8,
    [ASPEED_DEV_I2C]    = 12,
    [ASPEED_DEV_PECI]   = 15,
    [ASPEED_DEV_ETH1]   = 2,
    [ASPEED_DEV_ETH2]   = 3,
    [ASPEED_DEV_XDMA]   = 6,
    [ASPEED_DEV_SDHCI]  = 26,
    [ASPEED_DEV_HACE]   = 4,
    /* AST2050/AST1100 datasheet §10 p.99: Video Engine = INT#7 (the same
     * source number the aspeed-g4.dtsi video node uses on the AST2400). */
    [ASPEED_DEV_VIDEO]  = 7,
};

#define aspeed_soc_ast2500_irqmap aspeed_soc_ast2400_irqmap

static qemu_irq aspeed_soc_ast2400_get_irq(AspeedSoCState *s, int dev)
{
    Aspeed2400SoCState *a = ASPEED2400_SOC(s);
    AspeedSoCClass *sc = ASPEED_SOC_GET_CLASS(s);

    return qdev_get_gpio_in(DEVICE(&a->vic), sc->irqmap[dev]);
}

static void aspeed_ast2400_soc_init(Object *obj)
{
    Aspeed2400SoCState *a = ASPEED2400_SOC(obj);
    AspeedSoCState *s = ASPEED_SOC(obj);
    AspeedSoCClass *sc = ASPEED_SOC_GET_CLASS(s);
    int i;
    const char *socname;
    char socname_buf[8];
    char typename[64];

    if (sc->qom_socname) {
        /* SoC overrides the child-device socname (e.g. AST2050 -> ast2400) */
        socname = sc->qom_socname;
    } else if (sscanf(object_get_typename(obj), "%7s", socname_buf) == 1) {
        socname = socname_buf;
    } else {
        g_assert_not_reached();
    }

    for (i = 0; i < sc->num_cpus; i++) {
        object_initialize_child(obj, "cpu[*]", &a->cpu[i],
                                aspeed_soc_cpu_type(sc));
    }

    /*
     * The AST2050 (G3) reuses the AST2400 child devices via qom_socname="ast2400",
     * but its SCU reset values and clock tree differ (datasheet §18), so it gets a
     * dedicated aspeed.scu-ast2050 model. Keyed on the AST2050 silicon revision so
     * only the G3 SoC selects it; the AST2400/2500 continue to use their own SCU.
     */
    if (sc->silicon_rev == AST2050_A1_SILICON_REV) {
        object_initialize_child(obj, "scu", &s->scu, TYPE_ASPEED_2050_SCU);
    } else {
        snprintf(typename, sizeof(typename), "aspeed.scu-%s", socname);
        object_initialize_child(obj, "scu", &s->scu, typename);
    }
    qdev_prop_set_uint32(DEVICE(&s->scu), "silicon-rev",
                         sc->silicon_rev);
    object_property_add_alias(obj, "hw-strap1", OBJECT(&s->scu),
                              "hw-strap1");
    object_property_add_alias(obj, "hw-strap2", OBJECT(&s->scu),
                              "hw-strap2");
    object_property_add_alias(obj, "hw-prot-key", OBJECT(&s->scu),
                              "hw-prot-key");

    /*
     * The faithful single-bank G3 VIC (TYPE_ASPEED_2050_VIC) register model is
     * HARDWARE-CONFIRMED against the real KGPE-D16 AST2050 over JTAG: sense/dual/
     * event reset to 0 and are fully writable (the AST2400 VIC hardwires them
     * non-zero/read-only). See qemu-model/results/vic-hardware-crosscheck.md.
     *
     * WIRED for the AST2050. Wiring it earlier hung the proprietary C410X boot (C4)
     * -- the vendor firmware WDT-reset at ~17 s -- but that was NOT a VIC bug: the
     * root cause was the timer model. QEMU's aspeed_timer toggled its IRQ line each
     * expiry, which only yields one interrupt per expiry when the VIC is dual-edge
     * (as the AST2400 hardwires for timers 16-18). The faithful G3 VIC resets
     * dual-edge to 0 and both the vendor firmware and our irq-aspeed-g3-vic driver
     * program the timer as a single rising-edge source, so the toggle latched only
     * every OTHER expiry -> HZ/2 -> the vendor watchdog daemon lost its race with
     * the wall-clock WDT. Fixed in hw/timer/aspeed_timer.c (one rising-edge pulse
     * per expiry on the AST2050). C4 now boots its BMC web service and our modern
     * kernel (irq-aspeed-g3-vic) boots to SSH, both on the faithful G3 VIC. See
     * qemu-model/results/vic-hardware-crosscheck.md §7.
     */
    if (sc->silicon_rev == AST2050_A1_SILICON_REV) {
        object_initialize_child(obj, "vic", &a->vic, TYPE_ASPEED_2050_VIC);
    } else {
        object_initialize_child(obj, "vic", &a->vic, TYPE_ASPEED_VIC);
    }

    /*
     * The AST2050 (G3) has a counter-style RTC (created in realize); AST2400/2500
     * keep the BCD/CMOS aspeed_rtc. Only create the stub when it will be realized.
     */
    if (sc->silicon_rev != AST2050_A1_SILICON_REV) {
        object_initialize_child(obj, "rtc", &s->rtc, TYPE_ASPEED_RTC);
    }

    snprintf(typename, sizeof(typename), "aspeed.timer-%s", socname);
    object_initialize_child(obj, "timerctrl", &s->timerctrl, typename);

    /*
     * The AST2050 (G3) has NO ADC block: the ADC (0x1E6E9000) was introduced with
     * the AST2400/G4 (datasheet §1.4 p27 feature comparison + §9 memory map — see
     * qemu-model/AST2050-MEMORY-MAP.md, which records "ADC ... Absent ... introduced
     * with the AST2400"). The shared AST2400 base creates it unconditionally; skip it
     * on the G3 so the model does not present a peripheral the real silicon lacks (a
     * guest access to 0x1E6E9000 then reads as unassigned, exactly like the hardware).
     */
    if (sc->silicon_rev != AST2050_A1_SILICON_REV) {
        snprintf(typename, sizeof(typename), "aspeed.adc-%s", socname);
        object_initialize_child(obj, "adc", &s->adc, typename);
    }

    snprintf(typename, sizeof(typename), "aspeed.i2c-%s", socname);
    object_initialize_child(obj, "i2c", &s->i2c, typename);

    object_initialize_child(obj, "peci", &s->peci, TYPE_ASPEED_PECI);

    snprintf(typename, sizeof(typename), "aspeed.fmc-%s", socname);
    object_initialize_child(obj, "fmc", &s->fmc, typename);

    for (i = 0; i < sc->spis_num; i++) {
        snprintf(typename, sizeof(typename), "aspeed.spi%d-%s", i + 1, socname);
        object_initialize_child(obj, "spi[*]", &s->spi[i], typename);
    }

    /*
     * The AST2050 (G3) has NO EHCI USB host controller — it is an AST2400+
     * block. Don't create it, so the faithful machine exposes no phantom EHCI
     * at 0x1E6A1000/0x1E6A3000 (all USB is via the device/vhub at 0x1E6A0000).
     * Gate _init and realize identically: an un-inited child that realize then
     * tried to map would trip qdev's realized-properly assertion.
     */
    for (i = 0; i < sc->ehcis_num
             && sc->silicon_rev != AST2050_A1_SILICON_REV; i++) {
        object_initialize_child(obj, "ehci[*]", &s->ehci[i],
                                TYPE_PLATFORM_EHCI);
    }

    /*
     * The AST2050 (G3) memory controller is DDR2, not the AST2400 DDR3. Its MCR04
     * config resets to 0 (firmware writes the geometry; no SPD/strap/probe sizing),
     * stores writes verbatim, and exposes the AST2000-compat MCR100 shadow (reads
     * 0xA8). The real KGPE-D16 MCR04=0x00000585 (4-bank/64 MB) was captured over
     * JTAG. See qemu-model/peripherals/sdram/DATASHEET-SDRAM.md (datasheet §17).
     * Gate on the G3 silicon rev; AST2400/2500 keep the DDR3 aspeed_sdmc.
     */
    if (sc->silicon_rev == AST2050_A1_SILICON_REV) {
        object_initialize_child(obj, "sdmc", &s->sdmc, TYPE_ASPEED_2050_SDMC);
    } else {
        snprintf(typename, sizeof(typename), "aspeed.sdmc-%s", socname);
        object_initialize_child(obj, "sdmc", &s->sdmc, typename);
    }
    object_property_add_alias(obj, "ram-size", OBJECT(&s->sdmc),
                              "ram-size");

    for (i = 0; i < sc->wdts_num; i++) {
        snprintf(typename, sizeof(typename), "aspeed.wdt-%s", socname);
        object_initialize_child(obj, "wdt[*]", &s->wdt[i], typename);
    }

    for (i = 0; i < sc->macs_num; i++) {
        object_initialize_child(obj, "ftgmac100[*]", &s->ftgmac100[i],
                                TYPE_FTGMAC100);
    }

    for (i = 0; i < sc->uarts_num; i++) {
        object_initialize_child(obj, "uart[*]", &s->uart[i], TYPE_SERIAL_MM);
    }

    /*
     * AST2050 (G3) host-facing VUART (0x1E787000): a virtual 16550 the host
     * reaches over LPC (I/O 0x3F8, datasheet §29) and the BMC bridges to
     * Serial-over-LAN.  Modeled as a plain SerialMM 16550 (the LPC-side
     * address/SIRQ control registers at offset 0x20+ are not decoded — there is
     * no LPC host peer in this machine, so the BMC-side 16550 is what matters).
     */
    if (sc->has_vuart) {
        object_initialize_child(obj, "vuart", &a->vuart, TYPE_SERIAL_MM);
    }

    snprintf(typename, sizeof(typename), TYPE_ASPEED_XDMA "-%s", socname);
    object_initialize_child(obj, "xdma", &s->xdma, typename);

    snprintf(typename, sizeof(typename), "aspeed.gpio-%s", socname);
    object_initialize_child(obj, "gpio", &s->gpio, typename);

    snprintf(typename, sizeof(typename), "aspeed.sdhci-%s", socname);
    object_initialize_child(obj, "sdc", &s->sdhci, typename);

    object_property_set_int(OBJECT(&s->sdhci), "num-slots", 2, &error_abort);

    /* Init sd card slot class here so that they're under the correct parent */
    for (i = 0; i < ASPEED_SDHCI_NUM_SLOTS; ++i) {
        object_initialize_child(obj, "sdhci[*]", &s->sdhci.slots[i],
                                TYPE_SYSBUS_SDHCI);
    }

    /*
     * The AST2050 (G3) uses its own LPC layout (aspeed.lpc-ast2050, created in
     * realize); the AST2400 aspeed_lpc puts KCS/iBT at the wrong 0x140 offsets.
     * Gate the AST2400 LPC out of _init too, or realize would assert on an
     * un-realized child.
     */
    if (sc->silicon_rev != AST2050_A1_SILICON_REV) {
        object_initialize_child(obj, "lpc", &s->lpc, TYPE_ASPEED_LPC);
    }

    snprintf(typename, sizeof(typename), "aspeed.hace-%s", socname);
    object_initialize_child(obj, "hace", &s->hace, typename);

    object_initialize_child(obj, "iomem", &s->iomem, TYPE_UNIMPLEMENTED_DEVICE);
    /*
     * The AST2050 (G3) gets a real video engine (created in realize); the
     * AST2400/2500 keep the unimplemented stub. Only create the stub when it will
     * be realized, or qdev's realize assertion fires.
     */
    if (sc->silicon_rev != AST2050_A1_SILICON_REV) {
        object_initialize_child(obj, "video", &s->video, TYPE_UNIMPLEMENTED_DEVICE);
    }
}

/*
 * AST2050 (G3) SCU clock-stop / reset-hold side effects — see the wiring
 * comment at the end of realize. Level 1 = clock stopped / held in reset.
 */
static void aspeed_2050_uartclk_stop(void *opaque, int n, int level)
{
    AspeedSoCState *s = ASPEED_SOC(opaque);

    /* SCU0C[15]: ONE gate for both G3 UARTs (UART1 + the 0x1E784000 console) */
    memory_region_set_enabled(
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->uart[0]), 0), !level);
    memory_region_set_enabled(
        sysbus_mmio_get_region(
            SYS_BUS_DEVICE(&s->uart[ASPEED_DEV_UART5 - ASPEED_DEV_UART1]), 0),
        !level);
}

static void aspeed_2050_lclk_stop(void *opaque, int n, int level)
{
    Aspeed2400SoCState *a = ASPEED2400_SOC(opaque);

    /* SCU0C[8]: LPC controller clock (G3 LPC model incl. KCS channels) */
    memory_region_set_enabled(
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&a->lpc_g3), 0), !level);
}

static void aspeed_2050_i2c_rst(void *opaque, int n, int level)
{
    AspeedSoCState *s = ASPEED_SOC(opaque);

    /*
     * SCU04[2]: I2C/SMBus controller reset hold (all 7 engines). While held,
     * the register file is inert; internal controller state is not modelled
     * across an assert/deassert cycle (the device model's own reset covers
     * the cold-boot path, which is the case both Linux and the vendor
     * firmware exercise: de-assert once at init, before programming).
     */
    memory_region_set_enabled(
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->i2c), 0), !level);
}

static void aspeed_ast2400_soc_realize(DeviceState *dev, Error **errp)
{
    int i;
    Aspeed2400SoCState *a = ASPEED2400_SOC(dev);
    AspeedSoCState *s = ASPEED_SOC(dev);
    AspeedSoCClass *sc = ASPEED_SOC_GET_CLASS(s);
    g_autofree char *sram_name = NULL;

    /* Default boot region (SPI memory or ROMs) */
    memory_region_init(&s->spi_boot_container, OBJECT(s),
                       "aspeed.spi_boot_container", 0x10000000);
    memory_region_add_subregion(s->memory, sc->memmap[ASPEED_DEV_SPI_BOOT],
                                &s->spi_boot_container);

    /* IO space */
    aspeed_mmio_map_unimplemented(s, SYS_BUS_DEVICE(&s->iomem), "aspeed.io",
                                  sc->memmap[ASPEED_DEV_IOMEM],
                                  ASPEED_SOC_IOMEM_SIZE);

    /*
     * Video engine. The AST2050 (G3) has a real video engine (KVM screen
     * capture) that OpenBMC's aspeed-video driver uses; give it a real device
     * (VR000 protection-key + capture datapath). AST2400/2500 keep the
     * unimplemented stub. The engine reads its "internal VGA" source out of the
     * VGA carve-out at the top of DRAM, whose size comes from the SCU70[3:2]
     * strap (datasheet §18.2 p.217, referenced by MCR04[5:4] §17 p.185:
     * 00=8MB 01=16MB 10=32MB 11=64MB), and DMAs the compressed stream into the
     * driver-programmed buffers (M-Bus, §20.2 p.232). The completion IRQ
     * (INT#7) is wired to the VIC after the VIC is realized below.
     * See qemu-model/peripherals/video.
     */
    if (sc->silicon_rev == AST2050_A1_SILICON_REV) {
        uint32_t strap1 = object_property_get_uint(OBJECT(&s->scu), "hw-strap1",
                                                   &error_abort);
        uint32_t vga_mem_size = (8 * MiB) << SCU_HW_STRAP_VGA_SIZE_GET(strap1);

        object_initialize_child(OBJECT(dev), "video-g3", &a->video_g3,
                                TYPE_ASPEED_VIDEO_AST2050);
        object_property_set_link(OBJECT(&a->video_g3), "dram",
                                 OBJECT(s->dram_mr), &error_abort);
        object_property_set_uint(OBJECT(&a->video_g3), "dram-base",
                                 sc->memmap[ASPEED_DEV_SDRAM], &error_abort);
        object_property_set_uint(OBJECT(&a->video_g3), "vga-mem-size",
                                 vga_mem_size, &error_abort);
        if (!sysbus_realize(SYS_BUS_DEVICE(&a->video_g3), errp)) {
            return;
        }
        aspeed_mmio_map(s, SYS_BUS_DEVICE(&a->video_g3), 0,
                        sc->memmap[ASPEED_DEV_VIDEO]);
    } else {
        aspeed_mmio_map_unimplemented(s, SYS_BUS_DEVICE(&s->video), "aspeed.video",
                                      sc->memmap[ASPEED_DEV_VIDEO], 0x1000);
    }

    /* CPU */
    for (i = 0; i < sc->num_cpus; i++) {
        object_property_set_link(OBJECT(&a->cpu[i]), "memory",
                                 OBJECT(s->memory), &error_abort);
        if (!qdev_realize(DEVICE(&a->cpu[i]), NULL, errp)) {
            return;
        }
    }

    /* SRAM */
    sram_name = g_strdup_printf("aspeed.sram.%d", CPU(&a->cpu[0])->cpu_index);
    if (!memory_region_init_ram(&s->sram, OBJECT(s), sram_name, sc->sram_size,
                                errp)) {
        return;
    }
    memory_region_add_subregion(s->memory,
                                sc->memmap[ASPEED_DEV_SRAM], &s->sram);

    /* SCU */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->scu), errp)) {
        return;
    }
    aspeed_mmio_map(s, SYS_BUS_DEVICE(&s->scu), 0, sc->memmap[ASPEED_DEV_SCU]);

    /* VIC */
    if (!sysbus_realize(SYS_BUS_DEVICE(&a->vic), errp)) {
        return;
    }
    aspeed_mmio_map(s, SYS_BUS_DEVICE(&a->vic), 0, sc->memmap[ASPEED_DEV_VIC]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&a->vic), 0,
                       qdev_get_gpio_in(DEVICE(&a->cpu), ARM_CPU_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&a->vic), 1,
                       qdev_get_gpio_in(DEVICE(&a->cpu), ARM_CPU_FIQ));

    /*
     * AST2050 (G3) video engine completion IRQ: INT#7 (datasheet §10 p.99),
     * deferred from the video realize above because the VIC did not exist yet.
     */
    if (sc->silicon_rev == AST2050_A1_SILICON_REV) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&a->video_g3), 0,
                           aspeed_soc_get_irq(s, ASPEED_DEV_VIDEO));
    }

    /*
     * AST2050 (G3) PWM/tachometer. Mainline QEMU leaves 0x1E786000 unmapped; the
     * real AST2050 has 4 PWM + 16 tach here, which OpenBMC uses for fan hwmon. A
     * dedicated G3 device, keyed on the silicon revision so AST2400/2500 are
     * unchanged. See qemu-model/peripherals/pwm.
     */
    if (sc->silicon_rev == AST2050_A1_SILICON_REV) {
        object_initialize_child(OBJECT(dev), "pwm", &a->pwm_g3,
                                TYPE_ASPEED_PWM_AST2050);
        if (!sysbus_realize(SYS_BUS_DEVICE(&a->pwm_g3), errp)) {
            return;
        }
        aspeed_mmio_map(s, SYS_BUS_DEVICE(&a->pwm_g3), 0,
                        sc->memmap[ASPEED_DEV_PWM]);
        sysbus_connect_irq(SYS_BUS_DEVICE(&a->pwm_g3), 0,
                           aspeed_soc_get_irq(s, ASPEED_DEV_PWM));
    }

    /* RTC */
    if (sc->silicon_rev == AST2050_A1_SILICON_REV) {
        /* AST2050 (G3) counter-style RTC; see qemu-model/peripherals/rtc. */
        object_initialize_child(OBJECT(dev), "rtc-g3", &a->rtc_g3,
                                TYPE_ASPEED_RTC_AST2050);
        if (!sysbus_realize(SYS_BUS_DEVICE(&a->rtc_g3), errp)) {
            return;
        }
        aspeed_mmio_map(s, SYS_BUS_DEVICE(&a->rtc_g3), 0,
                        sc->memmap[ASPEED_DEV_RTC]);
        sysbus_connect_irq(SYS_BUS_DEVICE(&a->rtc_g3), 0,
                           aspeed_soc_get_irq(s, ASPEED_DEV_RTC));
    } else {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->rtc), errp)) {
            return;
        }
        aspeed_mmio_map(s, SYS_BUS_DEVICE(&s->rtc), 0, sc->memmap[ASPEED_DEV_RTC]);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->rtc), 0,
                           aspeed_soc_get_irq(s, ASPEED_DEV_RTC));
    }

    /*
     * AST2050 (G3) legacy SMC (SPI flash controller): control registers at
     * 0x16000000 and the CE2 SPI flash window at 0x14000000. Mainline QEMU models
     * only the AST2400 FMC (0x1E620000); the G3's legacy SMC is a distinct block.
     * The vendor firmware reads the flash JEDEC ID via UMA (user mode) byte-banging
     * the 0x14000000 window; the window forwards each byte to the on-board
     * mx25l12805d (JEDEC 0xC22018), so the ID reads correctly instead of 0 (which
     * previously caused a non-fatal div0 in aess_write_spi_nor_flash). Our modern
     * kernel uses the FMC, not this legacy SMC. See qemu-model/peripherals/smc.
     */
    if (sc->silicon_rev == AST2050_A1_SILICON_REV) {
        object_initialize_child(OBJECT(dev), "smc-g3", &a->smc_g3,
                                TYPE_ASPEED_SMC_AST2050);
        if (!sysbus_realize(SYS_BUS_DEVICE(&a->smc_g3), errp)) {
            return;
        }
        aspeed_mmio_map(s, SYS_BUS_DEVICE(&a->smc_g3), 0, 0x16000000);
        aspeed_mmio_map(s, SYS_BUS_DEVICE(&a->smc_g3), 1, 0x10000000);
    }

    /*
     * AST2050 (G3) USB2.0 device / virtual-hub controller at 0x1E6A0000 (the BMC
     * virtual-media / virtual-HID datapath). Mainline QEMU leaves this unmapped;
     * the G3 has no EHCI so all USB is via this block. See qemu-model/peripherals/usb.
     */
    if (sc->silicon_rev == AST2050_A1_SILICON_REV) {
        object_initialize_child(OBJECT(dev), "udc-g3", &a->udc_g3,
                                TYPE_ASPEED_UDC_AST2050);
        if (!sysbus_realize(SYS_BUS_DEVICE(&a->udc_g3), errp)) {
            return;
        }
        aspeed_mmio_map(s, SYS_BUS_DEVICE(&a->udc_g3), 0, 0x1E6A0000);
        /*
         * VIC INT#5 (datasheet Sec 10 p.99; DTS interrupts = <5>). Needed so the
         * model's fatal "USB command bus dead-lock" (ISR[18]) reaches the CPU --
         * the G3 vhub hazard the mainline driver livelocks on and kernel patch
         * 0007 fixes. Without this the deadlock IRQ would go nowhere.
         */
        sysbus_connect_irq(SYS_BUS_DEVICE(&a->udc_g3), 0,
                           qdev_get_gpio_in(DEVICE(&a->vic), 5));
    }

    /* Timer */
    object_property_set_link(OBJECT(&s->timerctrl), "scu", OBJECT(&s->scu),
                             &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->timerctrl), errp)) {
        return;
    }
    aspeed_mmio_map(s, SYS_BUS_DEVICE(&s->timerctrl), 0,
                    sc->memmap[ASPEED_DEV_TIMER1]);
    for (i = 0; i < ASPEED_TIMER_NR_TIMERS; i++) {
        qemu_irq irq = aspeed_soc_get_irq(s, ASPEED_DEV_TIMER1 + i);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->timerctrl), i, irq);
    }

    /* ADC -- absent on the G3 (AST2050); see the instance-init note above. */
    if (sc->silicon_rev != AST2050_A1_SILICON_REV) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->adc), errp)) {
            return;
        }
        aspeed_mmio_map(s, SYS_BUS_DEVICE(&s->adc), 0, sc->memmap[ASPEED_DEV_ADC]);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->adc), 0,
                           aspeed_soc_get_irq(s, ASPEED_DEV_ADC));
    }

    /* UART */
    if (!aspeed_soc_uart_realize(s, errp)) {
        return;
    }

    /* VUART (AST2050/G3) — host Serial-over-LAN bridge; see the soc_init note. */
    if (sc->has_vuart) {
        SerialMM *smm = &a->vuart;

        qdev_prop_set_uint8(DEVICE(smm), "regshift", 2);
        qdev_prop_set_uint32(DEVICE(smm), "baudbase", 38400);
        qdev_set_legacy_instance_id(DEVICE(smm),
                                    sc->memmap[ASPEED_DEV_VUART], 2);
        qdev_prop_set_uint8(DEVICE(smm), "endianness", DEVICE_LITTLE_ENDIAN);
        /* Chardev set by the machine (serial_hd(1)). */
        if (!sysbus_realize(SYS_BUS_DEVICE(smm), errp)) {
            return;
        }
        sysbus_connect_irq(SYS_BUS_DEVICE(smm), 0,
                           aspeed_soc_get_irq(s, ASPEED_DEV_VUART));
        aspeed_mmio_map(s, SYS_BUS_DEVICE(smm), 0,
                        sc->memmap[ASPEED_DEV_VUART]);
    }

    /* I2C */
    object_property_set_link(OBJECT(&s->i2c), "dram", OBJECT(s->dram_mr),
                             &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->i2c), errp)) {
        return;
    }
    aspeed_mmio_map(s, SYS_BUS_DEVICE(&s->i2c), 0, sc->memmap[ASPEED_DEV_I2C]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->i2c), 0,
                       aspeed_soc_get_irq(s, ASPEED_DEV_I2C));

    /* PECI */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->peci), errp)) {
        return;
    }
    aspeed_mmio_map(s, SYS_BUS_DEVICE(&s->peci), 0,
                    sc->memmap[ASPEED_DEV_PECI]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->peci), 0,
                       aspeed_soc_get_irq(s, ASPEED_DEV_PECI));

    /* FMC, The number of CS is set at the board level */
    object_property_set_link(OBJECT(&s->fmc), "dram", OBJECT(s->dram_mr),
                             &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->fmc), errp)) {
        return;
    }
    aspeed_mmio_map(s, SYS_BUS_DEVICE(&s->fmc), 0, sc->memmap[ASPEED_DEV_FMC]);
    aspeed_mmio_map(s, SYS_BUS_DEVICE(&s->fmc), 1,
                    ASPEED_SMC_GET_CLASS(&s->fmc)->flash_window_base);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->fmc), 0,
                       aspeed_soc_get_irq(s, ASPEED_DEV_FMC));

    /* Set up an alias on the FMC CE0 region (boot default) */
    MemoryRegion *fmc0_mmio = &s->fmc.flashes[0].mmio;
    memory_region_init_alias(&s->spi_boot, OBJECT(s), "aspeed.spi_boot",
                             fmc0_mmio, 0, memory_region_size(fmc0_mmio));
    memory_region_add_subregion(&s->spi_boot_container, 0x0, &s->spi_boot);

    /* SPI */
    for (i = 0; i < sc->spis_num; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->spi[i]), errp)) {
            return;
        }
        aspeed_mmio_map(s, SYS_BUS_DEVICE(&s->spi[i]), 0,
                        sc->memmap[ASPEED_DEV_SPI1 + i]);
        aspeed_mmio_map(s, SYS_BUS_DEVICE(&s->spi[i]), 1,
                        ASPEED_SMC_GET_CLASS(&s->spi[i])->flash_window_base);
    }

    /* EHCI — absent on the AST2050 (G3); gated to match _init (no phantom). */
    for (i = 0; i < sc->ehcis_num
             && sc->silicon_rev != AST2050_A1_SILICON_REV; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->ehci[i]), errp)) {
            return;
        }
        aspeed_mmio_map(s, SYS_BUS_DEVICE(&s->ehci[i]), 0,
                        sc->memmap[ASPEED_DEV_EHCI1 + i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->ehci[i]), 0,
                           aspeed_soc_get_irq(s, ASPEED_DEV_EHCI1 + i));
    }

    /* SDMC - SDRAM Memory Controller */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sdmc), errp)) {
        return;
    }
    aspeed_mmio_map(s, SYS_BUS_DEVICE(&s->sdmc), 0,
                    sc->memmap[ASPEED_DEV_SDMC]);

    /* Watch dog */
    for (i = 0; i < sc->wdts_num; i++) {
        AspeedWDTClass *awc = ASPEED_WDT_GET_CLASS(&s->wdt[i]);
        hwaddr wdt_offset = sc->memmap[ASPEED_DEV_WDT] + i * awc->iosize;

        object_property_set_link(OBJECT(&s->wdt[i]), "scu", OBJECT(&s->scu),
                                 &error_abort);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->wdt[i]), errp)) {
            return;
        }
        aspeed_mmio_map(s, SYS_BUS_DEVICE(&s->wdt[i]), 0, wdt_offset);
    }

    /* RAM  */
    if (!aspeed_soc_dram_init(s, errp)) {
        return;
    }

    /* Net */
    for (i = 0; i < sc->macs_num; i++) {
        object_property_set_bool(OBJECT(&s->ftgmac100[i]), "aspeed", true,
                                 &error_abort);
        /*
         * AST2050 (G3): mark the MAC so its model applies the faithful G3
         * speed-mode behaviour (a MAC SW_RST clears the MACCR speed bit, and RX
         * frames are dropped when the MAC speed mode disagrees with the 100M
         * RMII link). This reproduces the real-silicon eth0 RX=0 seen with the
         * mainline ftgmac100 driver, whose preserve-only start_hw() leaves the
         * G3 MAC in 10M timing after the SW_RST. AST2400/2500/2600 keep the
         * default (off) -- their SW_RST preserves the speed bit.
         */
        if (sc->silicon_rev == AST2050_A1_SILICON_REV) {
            object_property_set_bool(OBJECT(&s->ftgmac100[i]), "aspeed-g3",
                                     true, &error_abort);
        }
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->ftgmac100[i]), errp)) {
            return;
        }
        aspeed_mmio_map(s, SYS_BUS_DEVICE(&s->ftgmac100[i]), 0,
                        sc->memmap[ASPEED_DEV_ETH1 + i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->ftgmac100[i]), 0,
                           aspeed_soc_get_irq(s, ASPEED_DEV_ETH1 + i));
    }

    /* XDMA */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->xdma), errp)) {
        return;
    }
    aspeed_mmio_map(s, SYS_BUS_DEVICE(&s->xdma), 0,
                    sc->memmap[ASPEED_DEV_XDMA]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->xdma), 0,
                       aspeed_soc_get_irq(s, ASPEED_DEV_XDMA));

    /* GPIO */
    /*
     * AST2050 (G3) only: enable the ASUS KGPE-D16 board power-sequencer glue in
     * the GPIO model so the OpenBMC host-power path (Redfish -> state-manager ->
     * GPIO request lines -> power-state input) is observable in emulation. The
     * property is off for every other Aspeed SoC/machine, so this is a no-op for
     * the AST2400/2500/2600 boards. See hw/gpio/aspeed_gpio.c
     * aspeed_gpio_kgpe_d16_pwrseq(). Set before realize (qdev property).
     */
    if (sc->silicon_rev == AST2050_A1_SILICON_REV) {
        object_property_set_bool(OBJECT(&s->gpio), "kgpe-d16-pwrseq", true,
                                 &error_abort);
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gpio), errp)) {
        return;
    }
    aspeed_mmio_map(s, SYS_BUS_DEVICE(&s->gpio), 0,
                    sc->memmap[ASPEED_DEV_GPIO]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->gpio), 0,
                       aspeed_soc_get_irq(s, ASPEED_DEV_GPIO));

    /* SDHCI */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sdhci), errp)) {
        return;
    }
    aspeed_mmio_map(s, SYS_BUS_DEVICE(&s->sdhci), 0,
                    sc->memmap[ASPEED_DEV_SDHCI]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->sdhci), 0,
                       aspeed_soc_get_irq(s, ASPEED_DEV_SDHCI));

    /* LPC */
    if (sc->silicon_rev != AST2050_A1_SILICON_REV) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->lpc), errp)) {
            return;
        }
        aspeed_mmio_map(s, SYS_BUS_DEVICE(&s->lpc), 0,
                        sc->memmap[ASPEED_DEV_LPC]);

        /* Connect the LPC IRQ to the VIC */
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->lpc), 0,
                           aspeed_soc_get_irq(s, ASPEED_DEV_LPC));

        /*
         * On the AST2400 and AST2500 the one LPC IRQ is shared between all of
         * the subdevices. Connect the LPC subdevice IRQs to the LPC controller
         * IRQ (by contrast, on the AST2600, the subdevice IRQs are connected
         * straight to the GIC).
         *
         * LPC subdevice IRQ sources are offset from 1 because the shared IRQ
         * output to the VIC is at offset 0.
         */
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->lpc), 1 + aspeed_lpc_kcs_1,
                           qdev_get_gpio_in(DEVICE(&s->lpc), aspeed_lpc_kcs_1));

        sysbus_connect_irq(SYS_BUS_DEVICE(&s->lpc), 1 + aspeed_lpc_kcs_2,
                           qdev_get_gpio_in(DEVICE(&s->lpc), aspeed_lpc_kcs_2));

        sysbus_connect_irq(SYS_BUS_DEVICE(&s->lpc), 1 + aspeed_lpc_kcs_3,
                           qdev_get_gpio_in(DEVICE(&s->lpc), aspeed_lpc_kcs_3));

        sysbus_connect_irq(SYS_BUS_DEVICE(&s->lpc), 1 + aspeed_lpc_kcs_4,
                           qdev_get_gpio_in(DEVICE(&s->lpc), aspeed_lpc_kcs_4));
    } else {
        /*
         * AST2050 (G3) LPC: KCS/BT/iLPC2AHB at the G3 register offsets
         * (0x24-0x8C), not the AST2400 0x140. Register-accurate model (there is
         * no LPC host in this machine); the shared LPC IRQ goes to the VIC.
         * See qemu-model/peripherals/lpc.
         */
        object_initialize_child(OBJECT(dev), "lpc-g3", &a->lpc_g3,
                                TYPE_ASPEED_LPC_AST2050);
        if (!sysbus_realize(SYS_BUS_DEVICE(&a->lpc_g3), errp)) {
            return;
        }
        aspeed_mmio_map(s, SYS_BUS_DEVICE(&a->lpc_g3), 0,
                        sc->memmap[ASPEED_DEV_LPC]);
        sysbus_connect_irq(SYS_BUS_DEVICE(&a->lpc_g3), 0,
                           aspeed_soc_get_irq(s, ASPEED_DEV_LPC));

        /*
         * AST2050 (G3) P2A PCI->AHB back door (the culvert `p2a` path). It has
         * no AHB-side register file (the P2A00/P2A04 control regs live behind
         * PCI-slave BAR1, host-side), so it is not MMIO-mapped; instead it
         * masters the AHB (linked to s->memory) to service the host aperture
         * cycles driven through its QOM back-channel. See
         * qemu-model/peripherals/p2a and hw/misc/aspeed_p2a_ast2050.c.
         */
        object_initialize_child(OBJECT(dev), "p2a-g3", &a->p2a_g3,
                                TYPE_ASPEED_P2A_AST2050);
        object_property_set_link(OBJECT(&a->p2a_g3), "ahb",
                                 OBJECT(s->memory), &error_abort);
        if (!sysbus_realize(SYS_BUS_DEVICE(&a->p2a_g3), errp)) {
            return;
        }
    }

    /* HACE */
    object_property_set_link(OBJECT(&s->hace), "dram", OBJECT(s->dram_mr),
                             &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->hace), errp)) {
        return;
    }
    aspeed_mmio_map(s, SYS_BUS_DEVICE(&s->hace), 0,
                    sc->memmap[ASPEED_DEV_HACE]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->hace), 0,
                       aspeed_soc_get_irq(s, ASPEED_DEV_HACE));

    /*
     * AST2050 (G3) clock-stop / reset-hold faithfulness (HW findings #94/#93).
     *
     * The G3 SCU drives three side-effect lines (see aspeed_scu.c):
     *  - SCU0C[15] "Stop UARTCLK" gates BOTH G3 UARTs — UART1 @0x1E783000 and
     *    UART2 @0x1E784000 (the model's UART1 and UART5 slots). Proven on
     *    silicon: the kernel's clk_disable_unused set this bit at t=4.16s and
     *    the live console died (HWPASS-PROGRESS.md §C.8, task #94).
     *  - SCU0C[8]  "Stop LCLK" gates the LPC controller (KCS/BT/snoop).
     *  - SCU04[2]  holds the whole 7-engine I2C controller in reset
     *    (reset default = held; firmware must de-assert before using I2C).
     *
     * A clock-dead / reset-held APB block's register file is inert: the MMIO
     * region is disabled so reads fall through to the background (return 0),
     * writes are dropped, and no IRQ can be raised — which is exactly how the
     * failure presents on real silicon (silent console; I2C never completes).
     */
    if (sc->silicon_rev == AST2050_A1_SILICON_REV) {
        qdev_connect_gpio_out_named(DEVICE(&s->scu), "g3-uartclk-stop", 0,
            qemu_allocate_irq(aspeed_2050_uartclk_stop, s, 0));
        qdev_connect_gpio_out_named(DEVICE(&s->scu), "g3-lclk-stop", 0,
            qemu_allocate_irq(aspeed_2050_lclk_stop, a, 0));
        qdev_connect_gpio_out_named(DEVICE(&s->scu), "g3-i2c-rst", 0,
            qemu_allocate_irq(aspeed_2050_i2c_rst, s, 0));
    }
}

static void aspeed_soc_ast2400_class_init(ObjectClass *oc, void *data)
{
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("arm926"),
        NULL
    };
    AspeedSoCClass *sc = ASPEED_SOC_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = aspeed_ast2400_soc_realize;
    /* Reason: Uses serial_hds and nd_table in realize() directly */
    dc->user_creatable = false;

    sc->valid_cpu_types = valid_cpu_types;
    sc->silicon_rev  = AST2400_A1_SILICON_REV;
    sc->sram_size    = 0x8000;
    sc->spis_num     = 1;
    sc->ehcis_num    = 1;
    sc->wdts_num     = 2;
    sc->macs_num     = 2;
    sc->uarts_num    = 5;
    sc->uarts_base   = ASPEED_DEV_UART1;
    sc->irqmap       = aspeed_soc_ast2400_irqmap;
    sc->memmap       = aspeed_soc_ast2400_memmap;
    sc->num_cpus     = 1;
    sc->get_irq      = aspeed_soc_ast2400_get_irq;
}

static void aspeed_soc_ast2500_class_init(ObjectClass *oc, void *data)
{
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("arm1176"),
        NULL
    };
    AspeedSoCClass *sc = ASPEED_SOC_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = aspeed_ast2400_soc_realize;
    /* Reason: Uses serial_hds and nd_table in realize() directly */
    dc->user_creatable = false;

    sc->valid_cpu_types = valid_cpu_types;
    sc->silicon_rev  = AST2500_A1_SILICON_REV;
    sc->sram_size    = 0x9000;
    sc->spis_num     = 2;
    sc->ehcis_num    = 2;
    sc->wdts_num     = 3;
    sc->macs_num     = 2;
    sc->uarts_num    = 5;
    sc->uarts_base   = ASPEED_DEV_UART1;
    sc->irqmap       = aspeed_soc_ast2500_irqmap;
    sc->memmap       = aspeed_soc_ast2500_memmap;
    sc->num_cpus     = 1;
    sc->get_irq      = aspeed_soc_ast2400_get_irq;
}

static void aspeed_soc_ast2050_class_init(ObjectClass *oc, void *data)
{
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("arm926"),
        NULL
    };
    AspeedSoCClass *sc = ASPEED_SOC_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = aspeed_ast2400_soc_realize;
    /* Reason: Uses serial_hds and nd_table in realize() directly */
    dc->user_creatable = false;

    sc->valid_cpu_types = valid_cpu_types;
    sc->silicon_rev  = AST2050_A1_SILICON_REV;
    /*
     * The AST2050 (G3) is register-compatible with the AST2400 (G4) for the
     * blocks QEMU models, so reuse the AST2400 peripheral device variants by
     * overriding the child socname.
     */
    sc->qom_socname  = "ast2400";
    sc->sram_size    = 0x8000;
    sc->spis_num     = 1;
    sc->ehcis_num    = 1;
    /*
     * The AST2050 (G3) integrates ONE watchdog timer (datasheet: "AST2050 /
     * AST1100 integrates one set of 32-bit programmable Watchdog Timer"), unlike
     * the AST2400 (2) / AST2500 (3). Modeling a 2nd WDT at 0x1E785020 is a G4
     * phantom. #144.
     */
    sc->wdts_num     = 1;
    sc->macs_num     = 2;
    sc->uarts_num    = 5;
    sc->uarts_base   = ASPEED_DEV_UART1;
    sc->has_vuart    = true;         /* host VUART @0x1E787000 for SOL (G3) */
    sc->irqmap       = aspeed_soc_ast2400_irqmap;
    sc->memmap       = aspeed_soc_ast2400_memmap;
    sc->num_cpus     = 1;
    sc->get_irq      = aspeed_soc_ast2400_get_irq;
}

static const TypeInfo aspeed_soc_ast2400_types[] = {
    {
        .name           = TYPE_ASPEED2400_SOC,
        .parent         = TYPE_ASPEED_SOC,
        .instance_init  = aspeed_ast2400_soc_init,
        .instance_size  = sizeof(Aspeed2400SoCState),
        .abstract       = true,
    }, {
        .name           = "ast2400-a1",
        .parent         = TYPE_ASPEED2400_SOC,
        .class_init     = aspeed_soc_ast2400_class_init,
    }, {
        .name           = "ast2500-a1",
        .parent         = TYPE_ASPEED2400_SOC,
        .class_init     = aspeed_soc_ast2500_class_init,
    }, {
        .name           = "ast2050-a1",
        .parent         = TYPE_ASPEED2400_SOC,
        .class_init     = aspeed_soc_ast2050_class_init,
    },
};

DEFINE_TYPES(aspeed_soc_ast2400_types)
