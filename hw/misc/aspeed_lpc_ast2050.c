/*
 * ASPEED AST2050 (G3) LPC host interface — KCS / BT / iLPC2AHB.
 *
 * Datasheet register layout at 0x1E789000 (see qemu-model/peripherals/lpc;
 * AST2050/AST1100 A3 datasheet V1.05 §30, p.311-326):
 *   HICR0-4   0x00-0x10  host-interface control (channel enables, IPMI ints)
 *   LADR3/12  0x14-0x20  host I/O addresses (LADR12L reset 0x60)
 *   IDR1-3    0x24-0x2C  KCS input data  (host->BMC; Slave R / Host W, p.315)
 *   ODR1-3    0x30-0x38  KCS output data (BMC->host; Slave RW / Host R, p.315)
 *   STR1-3    0x3C-0x44  KCS status (p.315-316; bit semantics below)
 *   BT        0x48-0x68  Block Transfer status/control/data
 *   SERIRQ    0x70-0x7C
 *   HICR5-8   0x80-0x8C  iLPC2AHB bridge (the culvert `ilpc` path)
 *   snoop     0x90-0x94  port-80h POST-code capture
 *
 * This is the G3 layout — NOT the AST2400 aspeed_lpc, which puts KCS/iBT at the
 * 0x140 offsets. The KCS programming model is H8S/2168-compatible (p.312).
 *
 * KCS handshake state machine (faithful to the STRn tables, p.315-316):
 *   bit0 OBF   Slave RW0C / Host R  — set by a BMC write to ODRn, cleared by a
 *                                     host read of the data port (ODRn).
 *   bit1 IBF   Slave R    / Host R  — set by a host write to the data or
 *                                     command port (IDRn), cleared by a BMC
 *                                     read of IDRn.
 *   bit3 C/D   Slave R    / Host R  — 1 if the host's last IDRn write was to
 *                                     the command port, 0 for the data port.
 *   bits 7:4,2 "Defined by user"     — Slave RW / Host R: BMC firmware places
 *                                     the IPMI KCS state (S1:S0) and SMS_ATN
 *                                     bits here (the datasheet does not
 *                                     hardwire an assignment).
 * IBF raises the LPC interrupt (VIC #8, high-level sensitive, §10 p.99) when
 * the channel's HICR2 IBFIE/IBFIF enable is set (p.313); a BMC read of IDRn
 * clears IBF and thus the line. There is no OBE interrupt (the kernel driver
 * polls STR.OBF), matching the silicon.
 *
 * There is no LPC host CPU in the kgpe-d16-bmc machine, so the HOST side of
 * each KCS channel (the LPC I/O ports at LADRn) is exposed as QOM properties,
 * mirroring mainline hw/misc/aspeed_lpc.c's idr/odr/str properties:
 *   host-kcs<N>-data    write = host OUT to the data port    (IDR, C/D=0)
 *                       read  = host IN  from the data port  (ODR, clears OBF)
 *   host-kcs<N>-cmdsts  write = host OUT to the command port (IDR, C/D=1)
 *                       read  = host IN  from the status port (STR)
 * These properties replace ONLY the LPC bus wires (the host I/O cycle decode
 * at LADRn); every handshake effect they trigger is the faithful STRn state
 * machine above. Accessing a channel the BMC has not enabled (HICR0 LPCnE;
 * plus HICR4 KCSENBL for channel 3, p.313-314) fails loudly — on the bus the
 * unclaimed cycle would simply not be answered.
 *
 * BT and the iLPC2AHB->AHB bridging remain register-file refinements.
 *
 * This code is licensed under the GPL version 2 or later.
 */
#include "qemu/osdep.h"
#include "hw/misc/aspeed_lpc_ast2050.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define TO_REG(o) ((o) >> 2)

#define R_HICR0    TO_REG(0x00)
#define R_HICR2    TO_REG(0x08)
#define R_HICR4    TO_REG(0x10)
#define R_IDR1     TO_REG(0x24)   /* IDR1..3 are consecutive (0x24/0x28/0x2C) */
#define R_ODR1     TO_REG(0x30)   /* ODR1..3 are consecutive (0x30/0x34/0x38) */
#define R_STR1     TO_REG(0x3C)   /* STR1..3 are consecutive (0x3C/0x40/0x44) */

/* HICR0 (p.313): LPC channel enables. */
#define HICR0_LPC1E        BIT(5)
#define HICR0_LPC2E        BIT(6)
#define HICR0_LPC3E        BIT(7)

/* HICR2 (p.313): IDRn receive-completion (IBF) interrupt enables, ch N=1..3. */
#define HICR2_IBFIE(ch)    BIT(1 + (ch))

/* HICR4 (p.314): channel #3 mode select. */
#define HICR4_KCSENBL      BIT(2)

/* STRn (p.315-316). */
#define STR_OBF            BIT(0)   /* Slave RW0C / Host R */
#define STR_IBF            BIT(1)   /* Slave R    / Host R */
#define STR_CMD_DAT        BIT(3)   /* Slave R    / Host R */
#define STR_DBU_MASK       0xF4     /* bits 7:4,2 "defined by user": Slave RW */

#define KCS_NR_CHANNELS    3

/* KCS channels are indexed 0..2 for KCS #1..#3 throughout. */

static bool aspeed_lpc_ast2050_kcs_enabled(AspeedLPCAST2050State *s, int ch)
{
    static const uint32_t en[KCS_NR_CHANNELS] = {
        HICR0_LPC1E, HICR0_LPC2E, HICR0_LPC3E,
    };

    if (!(s->regs[R_HICR0] & en[ch])) {
        return false;
    }
    /* Channel #3 is KCS-or-BT; KCS needs HICR4.KCSENBL too (p.314). */
    if (ch == 2 && !(s->regs[R_HICR4] & HICR4_KCSENBL)) {
        return false;
    }
    return true;
}

/*
 * LPC -> VIC #8 is a high-level-sensitive line (§10 p.99): asserted while any
 * enabled KCS channel has IBF set. Recomputed whenever IBF or an enable moves.
 */
static void aspeed_lpc_ast2050_update_irq(AspeedLPCAST2050State *s)
{
    int level = 0;
    int ch;

    for (ch = 0; ch < KCS_NR_CHANNELS; ch++) {
        if ((s->regs[R_STR1 + ch] & STR_IBF) &&
            (s->regs[R_HICR2] & HICR2_IBFIE(ch)) &&
            aspeed_lpc_ast2050_kcs_enabled(s, ch)) {
            level = 1;
        }
    }
    qemu_set_irq(s->irq, level);
}

static uint64_t aspeed_lpc_ast2050_read(void *opaque, hwaddr offset, unsigned size)
{
    AspeedLPCAST2050State *s = ASPEED_LPC_AST2050(opaque);
    unsigned reg = TO_REG(offset);

    if (reg >= ASPEED_LPC_AST2050_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds read at 0x%" HWADDR_PRIx
                      "\n", __func__, offset);
        return 0;
    }

    /* A BMC (slave) read of IDRn completes the receive: IBF clears (p.315). */
    if (reg >= R_IDR1 && reg < R_IDR1 + KCS_NR_CHANNELS) {
        int ch = reg - R_IDR1;

        s->regs[R_STR1 + ch] &= ~STR_IBF;
        aspeed_lpc_ast2050_update_irq(s);
    }

    return s->regs[reg];
}

static void aspeed_lpc_ast2050_write(void *opaque, hwaddr offset, uint64_t data,
                                     unsigned size)
{
    AspeedLPCAST2050State *s = ASPEED_LPC_AST2050(opaque);
    unsigned reg = TO_REG(offset);

    if (reg >= ASPEED_LPC_AST2050_NR_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds write at 0x%" HWADDR_PRIx
                      "\n", __func__, offset);
        return;
    }

    /* IDRn is Slave R / Host W (p.315): BMC-side writes are dropped. */
    if (reg >= R_IDR1 && reg < R_IDR1 + KCS_NR_CHANNELS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to host-owned KCS IDR 0x%"
                      HWADDR_PRIx "\n", __func__, offset);
        return;
    }

    /* A BMC write to ODRn posts a byte to the host: OBF sets (p.315). */
    if (reg >= R_ODR1 && reg < R_ODR1 + KCS_NR_CHANNELS) {
        int ch = reg - R_ODR1;

        s->regs[reg] = data & 0xFF;
        s->regs[R_STR1 + ch] |= STR_OBF;
        return;
    }

    /*
     * STRn (p.315-316): the "defined by user" bits (7:4,2 — where firmware
     * keeps the IPMI KCS state/SMS_ATN) are Slave RW; OBF is Slave RW0C
     * (write 0 to clear); IBF and C/D are Slave R (hardware-managed here).
     */
    if (reg >= R_STR1 && reg < R_STR1 + KCS_NR_CHANNELS) {
        uint32_t str = s->regs[reg];

        str = (str & ~STR_DBU_MASK) | (data & STR_DBU_MASK);
        if (!(data & STR_OBF)) {
            str &= ~STR_OBF;
        }
        s->regs[reg] = str;
        return;
    }

    s->regs[reg] = data;

    if (reg == R_HICR0 || reg == R_HICR2 || reg == R_HICR4) {
        aspeed_lpc_ast2050_update_irq(s);
    }
}

static const MemoryRegionOps aspeed_lpc_ast2050_ops = {
    .read = aspeed_lpc_ast2050_read,
    .write = aspeed_lpc_ast2050_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/*
 * The HOST half of a KCS channel: LPC I/O cycles at the channel's LADRn port
 * pair, driven through QOM properties (there is no host CPU in this machine).
 * Channel index is carried in the property opaque.
 */

static int aspeed_lpc_ast2050_host_kcs_channel(AspeedLPCAST2050State *s,
                                               void *opaque, Error **errp)
{
    int ch = (uintptr_t)opaque;

    if (!aspeed_lpc_ast2050_kcs_enabled(s, ch)) {
        error_setg(errp, "KCS channel %d is not enabled by the BMC "
                   "(HICR0.LPC%dE%s): the LPC cycle would not be claimed",
                   ch + 1, ch + 1, ch == 2 ? " + HICR4.KCSENBL" : "");
        return -1;
    }
    return ch;
}

/* Host OUT to the data (cmd=false) or command (cmd=true) port: IDR + IBF. */
static void aspeed_lpc_ast2050_host_kcs_out(AspeedLPCAST2050State *s, int ch,
                                            uint8_t val, bool cmd)
{
    uint32_t str = s->regs[R_STR1 + ch];

    s->regs[R_IDR1 + ch] = val;
    str |= STR_IBF;
    if (cmd) {
        str |= STR_CMD_DAT;
    } else {
        str &= ~STR_CMD_DAT;
    }
    s->regs[R_STR1 + ch] = str;
    aspeed_lpc_ast2050_update_irq(s);
}

static void aspeed_lpc_ast2050_host_kcs_set_data(Object *obj, Visitor *v,
                                                 const char *name, void *opaque,
                                                 Error **errp)
{
    AspeedLPCAST2050State *s = ASPEED_LPC_AST2050(obj);
    uint8_t val;
    int ch;

    if (!visit_type_uint8(v, name, &val, errp)) {
        return;
    }
    ch = aspeed_lpc_ast2050_host_kcs_channel(s, opaque, errp);
    if (ch < 0) {
        return;
    }
    aspeed_lpc_ast2050_host_kcs_out(s, ch, val, false);
}

static void aspeed_lpc_ast2050_host_kcs_set_cmd(Object *obj, Visitor *v,
                                                const char *name, void *opaque,
                                                Error **errp)
{
    AspeedLPCAST2050State *s = ASPEED_LPC_AST2050(obj);
    uint8_t val;
    int ch;

    if (!visit_type_uint8(v, name, &val, errp)) {
        return;
    }
    ch = aspeed_lpc_ast2050_host_kcs_channel(s, opaque, errp);
    if (ch < 0) {
        return;
    }
    aspeed_lpc_ast2050_host_kcs_out(s, ch, val, true);
}

/* Host IN from the data port: returns ODRn and clears OBF (p.315). */
static void aspeed_lpc_ast2050_host_kcs_get_data(Object *obj, Visitor *v,
                                                 const char *name, void *opaque,
                                                 Error **errp)
{
    AspeedLPCAST2050State *s = ASPEED_LPC_AST2050(obj);
    uint8_t val;
    int ch;

    ch = aspeed_lpc_ast2050_host_kcs_channel(s, opaque, errp);
    if (ch < 0) {
        return;
    }
    val = s->regs[R_ODR1 + ch] & 0xFF;
    s->regs[R_STR1 + ch] &= ~STR_OBF;
    visit_type_uint8(v, name, &val, errp);
}

/* Host IN from the command/status port: returns STRn (no side effect). */
static void aspeed_lpc_ast2050_host_kcs_get_status(Object *obj, Visitor *v,
                                                   const char *name,
                                                   void *opaque, Error **errp)
{
    AspeedLPCAST2050State *s = ASPEED_LPC_AST2050(obj);
    uint8_t val;
    int ch;

    ch = aspeed_lpc_ast2050_host_kcs_channel(s, opaque, errp);
    if (ch < 0) {
        return;
    }
    val = s->regs[R_STR1 + ch] & 0xFF;
    visit_type_uint8(v, name, &val, errp);
}

static void aspeed_lpc_ast2050_reset(DeviceState *dev)
{
    AspeedLPCAST2050State *s = ASPEED_LPC_AST2050(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[TO_REG(0x20)] = ASPEED_LPC_AST2050_LADR12L_RESET;   /* LADR12L */
    aspeed_lpc_ast2050_update_irq(s);
}

static void aspeed_lpc_ast2050_init(Object *obj)
{
    int ch;

    for (ch = 0; ch < KCS_NR_CHANNELS; ch++) {
        g_autofree char *data = g_strdup_printf("host-kcs%d-data", ch + 1);
        g_autofree char *cmdsts = g_strdup_printf("host-kcs%d-cmdsts", ch + 1);

        object_property_add(obj, data, "uint8",
                            aspeed_lpc_ast2050_host_kcs_get_data,
                            aspeed_lpc_ast2050_host_kcs_set_data,
                            NULL, (void *)(uintptr_t)ch);
        object_property_set_description(obj, data,
            "Host LPC I/O access to the KCS data port (LADRn): "
            "write = OUT to IDR (sets IBF, C/D=0); "
            "read = IN from ODR (clears OBF)");
        object_property_add(obj, cmdsts, "uint8",
                            aspeed_lpc_ast2050_host_kcs_get_status,
                            aspeed_lpc_ast2050_host_kcs_set_cmd,
                            NULL, (void *)(uintptr_t)ch);
        object_property_set_description(obj, cmdsts,
            "Host LPC I/O access to the KCS command/status port: "
            "write = OUT to IDR (sets IBF, C/D=1); "
            "read = IN from STR");
    }
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
    .instance_init = aspeed_lpc_ast2050_init,
    .class_init = aspeed_lpc_ast2050_class_init,
};

static void aspeed_lpc_ast2050_register_types(void)
{
    type_register_static(&aspeed_lpc_ast2050_info);
}

type_init(aspeed_lpc_ast2050_register_types)
