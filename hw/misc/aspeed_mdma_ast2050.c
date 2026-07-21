/*
 * ASPEED AST2050 (G3) MDMA — memory-copy / memory-fill DMA engine.
 *
 * Faithful model of the §22 register block (0x1E740000, IRQ6). The write to the
 * command register MDMA0C *fires* the command (there is no start bit, §22 note
 * p259): the engine copies (type 00) or fills (type 10) up to 16M-1 bytes, then
 * — if MDMA0C[31] was set — sets the per-ID done bit in MDMA14[23:16] and, when
 * that ID's mask in MDMA10[23:16] is enabled, asserts the (level-high, §10 Table
 * 36) IRQ6 line. Done/idle/overflow status bits are write-1-to-clear.
 *
 * The transfer uses the raw 28-bit source/dest addresses against the system
 * address space (faithful AHB decode). On real silicon the low 256 MB is SDRAM
 * once the AHBC boot-remap (AHBC8C[0]) is enabled; until that aperture is
 * modelled, transfers to the (unmapped) low window are silently dropped by the
 * memory core while the command/status/IRQ control path stays faithful.
 *
 * This code is licensed under the GPL version 2 or later.
 */
#include "qemu/osdep.h"
#include "hw/misc/aspeed_mdma_ast2050.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "exec/address-spaces.h"

#define MDMA_SRC     0x00
#define MDMA_DST     0x04
#define MDMA_FILL    0x08
#define MDMA_CMD     0x0C
#define MDMA_IRQ_CTL 0x10
#define MDMA_IRQ_STS 0x14

#define MDMA_ADDR_MASK   0x0FFFFFFF   /* [27:0] — 256 MB reach */
#define MDMA_CMD_UPDATE  BIT(31)      /* set per-ID status on completion */
#define MDMA_CMD_ID(c)   (((c) >> 28) & 0x7)
#define MDMA_CMD_TYPE(c) (((c) >> 24) & 0x3)
#define MDMA_CMD_LEN(c)  ((c) & 0xFFFFFF)  /* bytes; 0 = invalid */
#define MDMA_TYPE_COPY   0x0
#define MDMA_TYPE_FILL   0x2

/* MDMA14 status fields */
#define MDMA_STS_DONE_MASK 0x00FF0000  /* [23:16] per-ID done (W1C) */
#define MDMA_STS_IDLE      BIT(3)      /* idle (W1C) */
#define MDMA_STS_OVERFLOW  BIT(1)      /* queue overflow (W1C) */
#define MDMA_STS_RESET     0x00000100  /* [8:4]=16 (queue empty) at reset */
#define MDMA_STS_W1C       (MDMA_STS_DONE_MASK | MDMA_STS_IDLE | MDMA_STS_OVERFLOW)

/* MDMA10 control fields */
#define MDMA_CTL_ID_MASK   0x00FF0000  /* [23:16] per-ID irq mask */
#define MDMA_CTL_IRQ_IDLE  BIT(3)
#define MDMA_CTL_IRQ_OVF   BIT(1)

static void aspeed_mdma_update_irq(AspeedMDMAAST2050State *s)
{
    uint32_t sts = s->regs[MDMA_IRQ_STS >> 2];
    uint32_t ctl = s->regs[MDMA_IRQ_CTL >> 2];
    bool active = false;

    /* Per-ID: a done bit whose matching mask bit is enabled. */
    if ((sts & MDMA_STS_DONE_MASK) & (ctl & MDMA_CTL_ID_MASK)) {
        active = true;
    }
    if ((sts & MDMA_STS_IDLE) && (ctl & MDMA_CTL_IRQ_IDLE)) {
        active = true;
    }
    if ((sts & MDMA_STS_OVERFLOW) && (ctl & MDMA_CTL_IRQ_OVF)) {
        active = true;
    }
    qemu_set_irq(s->irq, active);
}

static void aspeed_mdma_do_command(AspeedMDMAAST2050State *s, uint32_t cmd)
{
    uint32_t type = MDMA_CMD_TYPE(cmd);
    uint32_t id = MDMA_CMD_ID(cmd);
    uint32_t len = MDMA_CMD_LEN(cmd);
    uint32_t src = s->regs[MDMA_SRC >> 2] & MDMA_ADDR_MASK;
    uint32_t dst = s->regs[MDMA_DST >> 2] & MDMA_ADDR_MASK;
    uint32_t fill = s->regs[MDMA_FILL >> 2];

    if (len == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: MDMA command with length 0 (invalid)\n",
                      __func__);
        return;
    }

    if (type == MDMA_TYPE_FILL) {
        /* Buffer fill: dword pattern over a dword-aligned range (writes only). */
        uint32_t off;
        for (off = 0; off + 4 <= len; off += 4) {
            uint32_t v = fill;
            address_space_write(&address_space_memory, dst + off,
                                MEMTXATTRS_UNSPECIFIED, &v, 4);
        }
        if (off < len) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: fill length %u is not dword-aligned (§22 requires "
                          "dword-aligned ranges); ignoring %u tail bytes\n",
                          __func__, len, len - off);
        }
    } else if (type == MDMA_TYPE_COPY) {
        uint8_t buf[256];
        uint32_t done = 0;
        while (done < len) {
            uint32_t chunk = MIN(len - done, (uint32_t)sizeof(buf));
            address_space_read(&address_space_memory, src + done,
                               MEMTXATTRS_UNSPECIFIED, buf, chunk);
            address_space_write(&address_space_memory, dst + done,
                                MEMTXATTRS_UNSPECIFIED, buf, chunk);
            done += chunk;
        }
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: reserved MDMA command type %u\n",
                      __func__, type);
        return;
    }

    /* Completion: set the per-ID done status if requested, then re-evaluate IRQ. */
    if (cmd & MDMA_CMD_UPDATE) {
        s->regs[MDMA_IRQ_STS >> 2] |= BIT(16 + id);
    }
    aspeed_mdma_update_irq(s);
}

static uint64_t aspeed_mdma_read(void *opaque, hwaddr offset, unsigned size)
{
    AspeedMDMAAST2050State *s = ASPEED_MDMA_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_MDMA_AST2050_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds read at 0x%" HWADDR_PRIx
                      "\n", __func__, offset);
        return 0;
    }
    return s->regs[reg];
}

static void aspeed_mdma_write(void *opaque, hwaddr offset, uint64_t data,
                              unsigned size)
{
    AspeedMDMAAST2050State *s = ASPEED_MDMA_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_MDMA_AST2050_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds write at 0x%" HWADDR_PRIx
                      "\n", __func__, offset);
        return;
    }

    switch (offset) {
    case MDMA_SRC:
    case MDMA_DST:
        s->regs[reg] = data & MDMA_ADDR_MASK;
        break;
    case MDMA_FILL:
        s->regs[reg] = data;
        break;
    case MDMA_CMD:
        /* The write itself fires the command (§22 note p259, no start bit). */
        s->regs[reg] = data;
        aspeed_mdma_do_command(s, data);
        break;
    case MDMA_IRQ_CTL:
        s->regs[reg] = data;
        aspeed_mdma_update_irq(s);
        break;
    case MDMA_IRQ_STS:
        /* W1C for [23:16] done, [3] idle, [1] overflow; RO [8:4],[0] preserved. */
        s->regs[reg] &= ~(data & MDMA_STS_W1C);
        aspeed_mdma_update_irq(s);
        break;
    default:
        s->regs[reg] = data;
        break;
    }
}

static const MemoryRegionOps aspeed_mdma_ops = {
    .read = aspeed_mdma_read,
    .write = aspeed_mdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void aspeed_mdma_reset(DeviceState *dev)
{
    AspeedMDMAAST2050State *s = ASPEED_MDMA_AST2050(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[MDMA_IRQ_STS >> 2] = MDMA_STS_RESET;   /* queue empty (16 dwords free) */
    qemu_set_irq(s->irq, 0);
}

static void aspeed_mdma_realize(DeviceState *dev, Error **errp)
{
    AspeedMDMAAST2050State *s = ASPEED_MDMA_AST2050(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &aspeed_mdma_ops, s,
                          TYPE_ASPEED_MDMA_AST2050, 0x20);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription vmstate_aspeed_mdma_ast2050 = {
    .name = TYPE_ASPEED_MDMA_AST2050,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, AspeedMDMAAST2050State,
                             ASPEED_MDMA_AST2050_NR_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static void aspeed_mdma_ast2050_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = aspeed_mdma_realize;
    device_class_set_legacy_reset(dc, aspeed_mdma_reset);
    dc->desc = "ASPEED AST2050 MDMA memory-copy/fill engine";
    dc->vmsd = &vmstate_aspeed_mdma_ast2050;
}

static const TypeInfo aspeed_mdma_ast2050_info = {
    .name = TYPE_ASPEED_MDMA_AST2050,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AspeedMDMAAST2050State),
    .class_init = aspeed_mdma_ast2050_class_init,
};

static void aspeed_mdma_ast2050_register_types(void)
{
    type_register_static(&aspeed_mdma_ast2050_info);
}

type_init(aspeed_mdma_ast2050_register_types)
