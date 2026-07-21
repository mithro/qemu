/*
 * ASPEED AST2050 (G3) MIC — Memory Integrity Check Engine (MICE).
 *
 * Faithful model of §13 (0x1E640000, IRQ1). When MIC0C[28] (enable) is written,
 * the engine scans pages 0..MIC0C[27:12] of DRAM (from address 0x0): for each
 * page whose 2-bit control-buffer entry (base MIC00) selects a check mode, it
 * reads the 4 KB page, computes a Fletcher-32 checksum, and either stores it into
 * the checksum buffer (base MIC04) when that entry is still the initiative value
 * (0), or flags a page error on mismatch and raises IRQ1 (level-high).
 *
 * The Fletcher-32 reduction matches the Raptor SLT (mictest.c do_chksum) that
 * byte-compares against the real silicon, so the checksum is bit-exact.
 *
 * Simplification: the real engine scans continuously at the MIC08 rate; here the
 * scan runs synchronously on each enable write (which models the first scan pass
 * the SLT relies on, and a re-scan on re-enable). Large page counts are therefore
 * slow but correct; no firmware here drives a full-DRAM continuous scan.
 *
 * This code is licensed under the GPL version 2 or later.
 */
#include "qemu/osdep.h"
#include "hw/misc/aspeed_mic_ast2050.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "exec/address-spaces.h"

#define MIC_CTRLBUF   0x00   /* control-buffer base (2 bits/page)  */
#define MIC_CHKSUMBUF 0x04   /* checksum-buffer base (4 bytes/page) */
#define MIC_RATE      0x08   /* scan rate control                  */
#define MIC_ENGCTRL   0x0C   /* engine control                     */
#define MIC_STOPPAGE  0x10   /* stop-page / TAG                    */
#define MIC_STATUS    0x14   /* status + interrupt mask            */
#define MIC_STATUS1   0x18   /* first page error                   */
#define MIC_STATUS2   0x1C   /* secondary page error               */

#define MIC_ENABLE       BIT(28)      /* MIC0C[28]: 1=enable, 0=reset       */
#define MIC_MAXPAGE_MASK 0x0FFFF000   /* MIC0C[27:12]: last page to check   */
#define MIC_ADDR_MASK    0x0FFFFFF8   /* MIC00/04[27:3]: 8-byte-aligned base */

/* MIC14 status */
#define MIC_STS_FIRST    BIT(28)      /* RO mirror of MIC18[28]     */
#define MIC_STS_SECOND   BIT(29)      /* RO mirror of MIC1C[29]     */
#define MIC_STS_LOST     BIT(30)      /* lost-page (overflow)       */
#define MIC_STS_INTMASK  0x00060000   /* [17:16] RW interrupt mask  */
#define MIC_STS_INT_FIRST  BIT(16)    /* enable IRQ on first error  */
#define MIC_STS_INT_SECOND BIT(17)    /* enable IRQ on second error */
#define MIC_STS_ERRPAGE  0x0000FFFF   /* [15:0] progress / err page */

#define MIC_STATUS1_FLAG BIT(28)      /* first-page-error flag (W1C)  */
#define MIC_STATUS2_FLAG BIT(29)      /* secondary-page-error (W1C)   */

/* 2-bit page control words (from the control buffer in DRAM) */
#define MIC_CTRL_SKIP  0x0   /* no read, no checksum, no error       */
#define MIC_CTRL_ECC   0x1   /* read only (lets SDMC ECC scrub)      */
#define MIC_CTRL_DEBUG 0x2   /* read + always write checksum         */
#define MIC_CTRL_MIC   0x3   /* read + check (store when initiative) */

/*
 * Fletcher-32 over one 4 KB page read from the system address space at page<<12.
 * Reduction structure is identical to the Raptor SLT (blocks of <=360 u16 words,
 * fold after each block, one final fold) so the result is bit-exact.
 */
static uint32_t aspeed_mic_fletcher32(uint32_t page)
{
    uint8_t buf[4096];
    uint32_t sum1 = 0xffff, sum2 = 0xffff;
    uint32_t len = 2048;   /* 16-bit words per 4 KB page */
    uint32_t j = 0;

    address_space_read(&address_space_memory, ((uint64_t)page) << 12,
                       MEMTXATTRS_UNSPECIFIED, buf, sizeof(buf));

    while (len) {
        uint32_t tlen = len > 360 ? 360 : len;
        len -= tlen;
        do {
            /* little-endian 16-bit word, as the ARM SLT reads *(u16 *) */
            uint16_t w = buf[2 * j] | ((uint16_t)buf[2 * j + 1] << 8);
            sum1 += w;
            sum2 += sum1;
            j++;
        } while (--tlen);
        sum1 = (sum1 & 0xffff) + (sum1 >> 16);
        sum2 = (sum2 & 0xffff) + (sum2 >> 16);
    }
    sum1 = (sum1 & 0xffff) + (sum1 >> 16);
    sum2 = (sum2 & 0xffff) + (sum2 >> 16);

    return (sum2 << 16) | sum1;
}

static void aspeed_mic_update_irq(AspeedMICAST2050State *s)
{
    uint32_t sts = s->regs[MIC_STATUS >> 2];
    bool first = s->regs[MIC_STATUS1 >> 2] & MIC_STATUS1_FLAG;
    bool second = s->regs[MIC_STATUS2 >> 2] & MIC_STATUS2_FLAG;
    bool active = false;

    /* Reflect the error flags into the read-only MIC14[29:28] mirror bits. */
    sts &= ~(MIC_STS_FIRST | MIC_STS_SECOND);
    if (first) {
        sts |= MIC_STS_FIRST;
    }
    if (second) {
        sts |= MIC_STS_SECOND;
    }
    s->regs[MIC_STATUS >> 2] = sts;

    /* Level-high IRQ1 while an enabled error flag is set (§10: sensitive high). */
    if ((sts & MIC_STS_INT_FIRST) && first) {
        active = true;
    }
    if ((sts & MIC_STS_INT_SECOND) && second) {
        active = true;
    }
    qemu_set_irq(s->irq, active);
}

static void aspeed_mic_page_error(AspeedMICAST2050State *s, uint32_t page)
{
    if (!(s->regs[MIC_STATUS1 >> 2] & MIC_STATUS1_FLAG)) {
        s->regs[MIC_STATUS1 >> 2] = MIC_STATUS1_FLAG | (page & MIC_STS_ERRPAGE);
    } else if (!(s->regs[MIC_STATUS2 >> 2] & MIC_STATUS2_FLAG)) {
        s->regs[MIC_STATUS2 >> 2] = MIC_STATUS2_FLAG | (page & MIC_STS_ERRPAGE);
    } else {
        /* A third error before the first two are cleared: lost-page overflow. */
        s->regs[MIC_STATUS >> 2] |= MIC_STS_LOST;
    }
    aspeed_mic_update_irq(s);
}

static void aspeed_mic_scan(AspeedMICAST2050State *s)
{
    uint32_t engctrl = s->regs[MIC_ENGCTRL >> 2];
    uint32_t last_page, ctrlbuf, chksumbuf, page;

    if (!(engctrl & MIC_ENABLE)) {
        return;
    }
    /* Guard: the scan reads/writes guest-programmed buffer addresses; if one
     * aliases this device's MMIO window the access re-enters here. */
    if (s->in_scan) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: re-entrant MIC scan (buffer aliases the MIC register "
                      "window?) dropped\n", __func__);
        return;
    }
    s->in_scan = true;

    last_page = (engctrl & MIC_MAXPAGE_MASK) >> 12;
    ctrlbuf = s->regs[MIC_CTRLBUF >> 2] & MIC_ADDR_MASK;
    chksumbuf = s->regs[MIC_CHKSUMBUF >> 2] & MIC_ADDR_MASK;

    for (page = 0; page <= last_page; page++) {
        uint8_t ctrl_byte;
        uint8_t ctrl;
        uint32_t computed, stored;

        /* progress = current page (MIC14[15:0]) */
        s->regs[MIC_STATUS >> 2] =
            (s->regs[MIC_STATUS >> 2] & ~MIC_STS_ERRPAGE) | (page & MIC_STS_ERRPAGE);

        /* 2-bit control word for this page (4 pages per control byte). */
        address_space_read(&address_space_memory, ctrlbuf + (page / 4),
                           MEMTXATTRS_UNSPECIFIED, &ctrl_byte, 1);
        ctrl = (ctrl_byte >> ((page % 4) * 2)) & 0x3;

        if (ctrl == MIC_CTRL_SKIP) {
            continue;
        }

        computed = aspeed_mic_fletcher32(page);

        if (ctrl == MIC_CTRL_ECC) {
            continue;   /* read-only mode: no checksum, no error */
        }

        stored = address_space_ldl_le(&address_space_memory, chksumbuf + page * 4,
                                      MEMTXATTRS_UNSPECIFIED, NULL);

        if (ctrl == MIC_CTRL_DEBUG) {
            /* debug mode: always (re)write the computed checksum, never error */
            address_space_stl_le(&address_space_memory, chksumbuf + page * 4,
                                  computed, MEMTXATTRS_UNSPECIFIED, NULL);
            continue;
        }

        /* MIC mode (0x3): initiative -> store; else check for mismatch. */
        if (stored == 0) {
            address_space_stl_le(&address_space_memory, chksumbuf + page * 4,
                                  computed, MEMTXATTRS_UNSPECIFIED, NULL);
        } else if (stored != computed) {
            aspeed_mic_page_error(s, page);
        }
    }

    s->in_scan = false;
}

static uint64_t aspeed_mic_read(void *opaque, hwaddr offset, unsigned size)
{
    AspeedMICAST2050State *s = ASPEED_MIC_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_MIC_AST2050_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds read at 0x%" HWADDR_PRIx
                      "\n", __func__, offset);
        return 0;
    }
    return s->regs[reg];
}

static void aspeed_mic_write(void *opaque, hwaddr offset, uint64_t data,
                             unsigned size)
{
    AspeedMICAST2050State *s = ASPEED_MIC_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_MIC_AST2050_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds write at 0x%" HWADDR_PRIx
                      "\n", __func__, offset);
        return;
    }

    switch (offset) {
    case MIC_ENGCTRL:
        s->regs[reg] = data;
        if (data & MIC_ENABLE) {
            aspeed_mic_scan(s);   /* first scan / re-scan on enable */
        }
        break;
    case MIC_STATUS:
        /* Only [17:16] interrupt mask is writable; [30:28] mirror + [15:0]
         * progress are read-only. */
        s->regs[reg] = (s->regs[reg] & ~MIC_STS_INTMASK) | (data & MIC_STS_INTMASK);
        aspeed_mic_update_irq(s);
        break;
    case MIC_STATUS1:
        if (data & MIC_STATUS1_FLAG) {          /* write-1-to-clear */
            s->regs[reg] &= ~MIC_STATUS1_FLAG;
        }
        aspeed_mic_update_irq(s);
        break;
    case MIC_STATUS2:
        if (data & MIC_STATUS2_FLAG) {          /* write-1-to-clear */
            s->regs[reg] &= ~MIC_STATUS2_FLAG;
        }
        aspeed_mic_update_irq(s);
        break;
    case MIC_STOPPAGE:
        s->regs[reg] = data;
        /*
         * §13.3: if the write-back TAG [31:16] is non-zero, MICE writes
         * {TAG,16'b0} into the checksum-buffer entry of page [15:0] (software
         * polls that entry to confirm a scan stop). The stop-scan-AT-page itself
         * is moot in this synchronous-scan model (a scan completes atomically on
         * enable); the observable TAG write-back is modelled here. The in_scan
         * guard covers the same alias-into-own-window re-entrancy hazard.
         */
        if ((data & 0xFFFF0000) && !s->in_scan) {
            uint32_t csum = s->regs[MIC_CHKSUMBUF >> 2] & MIC_ADDR_MASK;
            uint32_t page = data & 0xFFFF;
            s->in_scan = true;
            address_space_stl_le(&address_space_memory, csum + (uint64_t)page * 4,
                                 data & 0xFFFF0000, MEMTXATTRS_UNSPECIFIED, NULL);
            s->in_scan = false;
        }
        break;
    default:                                     /* MIC00/04/08 */
        s->regs[reg] = data;
        break;
    }
}

static const MemoryRegionOps aspeed_mic_ops = {
    .read = aspeed_mic_read,
    .write = aspeed_mic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void aspeed_mic_reset(DeviceState *dev)
{
    AspeedMICAST2050State *s = ASPEED_MIC_AST2050(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->in_scan = false;
    qemu_set_irq(s->irq, 0);
}

static void aspeed_mic_realize(DeviceState *dev, Error **errp)
{
    AspeedMICAST2050State *s = ASPEED_MIC_AST2050(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &aspeed_mic_ops, s,
                          TYPE_ASPEED_MIC_AST2050, 0x20);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription vmstate_aspeed_mic_ast2050 = {
    .name = TYPE_ASPEED_MIC_AST2050,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, AspeedMICAST2050State,
                             ASPEED_MIC_AST2050_NR_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static void aspeed_mic_ast2050_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = aspeed_mic_realize;
    device_class_set_legacy_reset(dc, aspeed_mic_reset);
    dc->desc = "ASPEED AST2050 Memory Integrity Check engine";
    dc->vmsd = &vmstate_aspeed_mic_ast2050;
}

static const TypeInfo aspeed_mic_ast2050_info = {
    .name = TYPE_ASPEED_MIC_AST2050,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AspeedMICAST2050State),
    .class_init = aspeed_mic_ast2050_class_init,
};

static void aspeed_mic_ast2050_register_types(void)
{
    type_register_static(&aspeed_mic_ast2050_info);
}

type_init(aspeed_mic_ast2050_register_types)
