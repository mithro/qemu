/*
 * ASPEED AST2050 (G3) LPC host interface — KCS / BT / iLPC2AHB.
 *
 * Datasheet register layout at 0x1E789000 (see qemu-model/peripherals/lpc):
 *   HICR0-4   0x00-0x10  host-interface control (channel enables, IPMI ints)
 *   LADR3/12  0x14-0x20  host I/O addresses (LADR12L reset 0x60)
 *   IDR1-3    0x24-0x2C  KCS input data  (host->BMC)
 *   ODR1-3    0x30-0x38  KCS output data (BMC->host)
 *   STR1-3    0x3C-0x44  KCS status (read-only; OBF/IBF/C-D set by KCS activity)
 *   BT        0x48-0x68  Block Transfer status/control/data
 *   SERIRQ    0x70-0x7C
 *   HICR5-8   0x80-0x8C  iLPC2AHB bridge (the culvert `ilpc` path)
 *   snoop     0x90-0x94  port-80h POST-code capture
 *
 * This is the G3 layout — NOT the AST2400 aspeed_lpc, which puts KCS/iBT at the
 * 0x140 offsets. Register-accurate model: config/data registers are RW; the KCS
 * status registers (STR1-3) are read-only (reset 0). Full KCS/BT OBF/IBF state
 * machines and the iLPC2AHB->AHB bridging are refinements; there is no LPC host
 * in this machine, so the registers are the observable surface (the vendor
 * firmware and OpenBMC's IPMI KCS/BT drivers bind here).
 *
 * This code is licensed under the GPL version 2 or later.
 */
#include "qemu/osdep.h"
#include "hw/misc/aspeed_lpc_ast2050.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

/* KCS status registers STR1-3 (0x3C/0x40/0x44) are read-only. */
static bool aspeed_lpc_ast2050_is_ro(unsigned reg)
{
    return reg == (0x3C >> 2) || reg == (0x40 >> 2) || reg == (0x44 >> 2);
}

static uint64_t aspeed_lpc_ast2050_read(void *opaque, hwaddr offset, unsigned size)
{
    AspeedLPCAST2050State *s = ASPEED_LPC_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_LPC_AST2050_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds read at 0x%" HWADDR_PRIx
                      "\n", __func__, offset);
        return 0;
    }
    return s->regs[reg];
}

static void aspeed_lpc_ast2050_write(void *opaque, hwaddr offset, uint64_t data,
                                     unsigned size)
{
    AspeedLPCAST2050State *s = ASPEED_LPC_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_LPC_AST2050_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds write at 0x%" HWADDR_PRIx
                      "\n", __func__, offset);
        return;
    }
    if (aspeed_lpc_ast2050_is_ro(reg)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to read-only KCS status 0x%"
                      HWADDR_PRIx "\n", __func__, offset);
        return;
    }
    s->regs[reg] = data;
}

static const MemoryRegionOps aspeed_lpc_ast2050_ops = {
    .read = aspeed_lpc_ast2050_read,
    .write = aspeed_lpc_ast2050_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void aspeed_lpc_ast2050_reset(DeviceState *dev)
{
    AspeedLPCAST2050State *s = ASPEED_LPC_AST2050(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[0x20 >> 2] = ASPEED_LPC_AST2050_LADR12L_RESET;   /* LADR12L */
}

static void aspeed_lpc_ast2050_realize(DeviceState *dev, Error **errp)
{
    AspeedLPCAST2050State *s = ASPEED_LPC_AST2050(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &aspeed_lpc_ast2050_ops, s,
                          TYPE_ASPEED_LPC_AST2050, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription vmstate_aspeed_lpc_ast2050 = {
    .name = TYPE_ASPEED_LPC_AST2050,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, AspeedLPCAST2050State,
                             ASPEED_LPC_AST2050_NR_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static void aspeed_lpc_ast2050_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = aspeed_lpc_ast2050_realize;
    device_class_set_legacy_reset(dc, aspeed_lpc_ast2050_reset);
    dc->desc = "ASPEED AST2050 LPC host interface (KCS/BT/iLPC2AHB)";
    dc->vmsd = &vmstate_aspeed_lpc_ast2050;
}

static const TypeInfo aspeed_lpc_ast2050_info = {
    .name = TYPE_ASPEED_LPC_AST2050,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AspeedLPCAST2050State),
    .class_init = aspeed_lpc_ast2050_class_init,
};

static void aspeed_lpc_ast2050_register_types(void)
{
    type_register_static(&aspeed_lpc_ast2050_info);
}

type_init(aspeed_lpc_ast2050_register_types)
