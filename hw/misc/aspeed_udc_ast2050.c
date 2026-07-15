/*
 * ASPEED AST2050 (G3) USB2.0 device / virtual-hub controller.
 *
 * Datasheet: base 0x1E6A0000, HUB00 root control at 0x00, device blocks and a
 * 21-endpoint pool (EPP#16-#20 at 0x300-0x34F) with DMA descriptors. This is the
 * BMC's virtual-media / virtual-HID datapath (OpenBMC obmc-ikvm). The AST2050 has
 * NO EHCI host (that's an AST2400+ block) — all USB is via this device/vhub.
 *
 * Faithful G3 semantics (datasheet Sec 15.3.2 p.156-160), the bits that differ
 * from the AST2400 the mainline aspeed-vhub driver targets:
 *   - HUB00[31] (PHY clock enable) is READ-ONLY — a PHY-clock-ready status. The
 *     driver *writes* it to "enable the PHY"; on the G3 that is a no-op.
 *   - The PHY stabilises only after reset is released (HUB00[11]) AND the driver
 *     polls the ready status; model that as HUB00[31] coming up once it is read
 *     after reset-release (a driver that connects without polling sees it stuck
 *     low). This is deterministic across host speeds, unlike a wall-clock timer
 *     (QEMU without icount does not advance virtual time through guest udelay()).
 *   - HUB0C[18] "USB command bus dead-lock" is a fatal, LEVEL-triggered, unmaskable
 *     interrupt: asserting HUB00[0] UPSTREAM_CONNECT into a not-yet-ready PHY
 *     latches it and it re-asserts until UPSTREAM_CONNECT is dropped. On silicon
 *     the mainline driver connects without waiting -> the level line never drops
 *     -> the CPU livelocks in the ISR (a hard boot hang). The G3 vhub driver port
 *     (kernel patch 0007) waits for HUB00[31] before connecting and drops
 *     UPSTREAM_CONNECT on ISR[18]; this model reproduces the hazard so that fix
 *     can be regression-gated in QEMU. See openbmc/bmc-functionality/VHUB-G3-PORT-PLAN.md.
 *
 * This code is licensed under the GPL version 2 or later.
 */
#include "qemu/osdep.h"
#include "hw/misc/aspeed_udc_ast2050.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

/* Root function registers (datasheet Sec 15.3.2; offsets == the vhub driver). */
#define UDC_CTRL        0x00    /* HUB00: root function control & status */
#define UDC_IER         0x08    /* HUB08: interrupt enable */
#define UDC_ISR         0x0C    /* HUB0C: interrupt status (W1C) */

#define CTRL_PHY_CLK        (1u << 31)  /* R/O on G3: PHY-clock-ready status */
#define CTRL_PHY_RESET_DIS  (1u << 11)  /* release PHY reset */
#define CTRL_UPSTREAM_CONNECT (1u << 0) /* pull up / present to host */

#define ISR_CMD_DEADLOCK    (1u << 18)  /* fatal, level-triggered, unmaskable */

static void aspeed_udc_ast2050_update_irq(AspeedUDCAST2050State *s)
{
    uint32_t isr = s->regs[UDC_ISR / 4];
    uint32_t ier = s->regs[UDC_IER / 4];
    /* ISR[18] (bus dead-lock) is unmaskable; everything else gates on IER. */
    bool level = (isr & ier) || (isr & ISR_CMD_DEADLOCK);

    qemu_set_irq(s->irq, level);
}

static uint64_t aspeed_udc_ast2050_read(void *opaque, hwaddr offset, unsigned size)
{
    AspeedUDCAST2050State *s = ASPEED_UDC_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_UDC_AST2050_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds read at 0x%" HWADDR_PRIx
                      "\n", __func__, offset);
        return 0;
    }
    if (offset == UDC_CTRL) {
        /* CTRL[31] is a read-only PHY-clock-ready status on the G3. */
        uint32_t val = (s->regs[reg] & ~CTRL_PHY_CLK) |
                       (s->phy_ready ? CTRL_PHY_CLK : 0);
        /*
         * The PHY comes ready once the driver polls this status after releasing
         * PHY reset -- so this read reports not-ready, and the NEXT read (the
         * driver's poll loop) sees ready. A driver that never polls (connects
         * blind) leaves it stuck low and latches the deadlock at connect.
         */
        if ((s->regs[reg] & CTRL_PHY_RESET_DIS) && !s->phy_ready) {
            s->phy_ready = true;
        }
        return val;
    }
    return s->regs[reg];
}

static void aspeed_udc_ast2050_write(void *opaque, hwaddr offset, uint64_t data,
                                     unsigned size)
{
    AspeedUDCAST2050State *s = ASPEED_UDC_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_UDC_AST2050_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds write at 0x%" HWADDR_PRIx
                      "\n", __func__, offset);
        return;
    }

    switch (offset) {
    case UDC_CTRL:
        /* CTRL[31] is read-only (PHY-clock-ready status). */
        data &= ~CTRL_PHY_CLK;
        s->regs[reg] = data;

        if (data & CTRL_UPSTREAM_CONNECT) {
            /* Connecting into a not-ready PHY latches the fatal deadlock. */
            if (!s->phy_ready) {
                s->deadlocked = true;
                s->regs[UDC_ISR / 4] |= ISR_CMD_DEADLOCK;
            }
        } else {
            /* Dropping the upstream connect recovers from the deadlock. */
            s->deadlocked = false;
            s->regs[UDC_ISR / 4] &= ~ISR_CMD_DEADLOCK;
        }
        aspeed_udc_ast2050_update_irq(s);
        return;

    case UDC_ISR:
        /* W1C. The deadlock is level-triggered: re-assert while it persists. */
        s->regs[reg] &= ~(uint32_t)data;
        if (s->deadlocked) {
            s->regs[reg] |= ISR_CMD_DEADLOCK;
        }
        aspeed_udc_ast2050_update_irq(s);
        return;

    case UDC_IER:
        s->regs[reg] = data;
        aspeed_udc_ast2050_update_irq(s);
        return;

    default:
        s->regs[reg] = data;
        return;
    }
}

static const MemoryRegionOps aspeed_udc_ast2050_ops = {
    .read = aspeed_udc_ast2050_read,
    .write = aspeed_udc_ast2050_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void aspeed_udc_ast2050_reset(DeviceState *dev)
{
    AspeedUDCAST2050State *s = ASPEED_UDC_AST2050(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->phy_ready = false;
    s->deadlocked = false;
}

static void aspeed_udc_ast2050_realize(DeviceState *dev, Error **errp)
{
    AspeedUDCAST2050State *s = ASPEED_UDC_AST2050(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &aspeed_udc_ast2050_ops, s,
                          TYPE_ASPEED_UDC_AST2050,
                          ASPEED_UDC_AST2050_NR_REGS * 4);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription vmstate_aspeed_udc_ast2050 = {
    .name = TYPE_ASPEED_UDC_AST2050,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(phy_ready, AspeedUDCAST2050State),
        VMSTATE_BOOL(deadlocked, AspeedUDCAST2050State),
        VMSTATE_UINT32_ARRAY(regs, AspeedUDCAST2050State,
                             ASPEED_UDC_AST2050_NR_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static void aspeed_udc_ast2050_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = aspeed_udc_ast2050_realize;
    device_class_set_legacy_reset(dc, aspeed_udc_ast2050_reset);
    dc->desc = "ASPEED AST2050 USB device/virtual-hub controller";
    dc->vmsd = &vmstate_aspeed_udc_ast2050;
}

static const TypeInfo aspeed_udc_ast2050_info = {
    .name = TYPE_ASPEED_UDC_AST2050,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AspeedUDCAST2050State),
    .class_init = aspeed_udc_ast2050_class_init,
};

static void aspeed_udc_ast2050_register_types(void)
{
    type_register_static(&aspeed_udc_ast2050_info);
}

type_init(aspeed_udc_ast2050_register_types)
