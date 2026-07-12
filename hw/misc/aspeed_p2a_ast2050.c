/*
 * ASPEED AST2050 (G3) P2A — PCI(P-Bus)-to-AHB back-door bridge.
 *
 * Faithful model of the AST2050/AST1100 P2A back door — the mechanism culvert's
 * `p2a` backend drives to peek/poke the whole SoC from a host PCI master
 * (validated on real silicon: SCU7C read back as 0x00000202 over P2A). Datasheet
 * AST2050/AST1100 A3 V1.05 §36 p.400 (P2A registers), §33 p.363-368 (PCI slave /
 * BAR1 = MMIOBASE), §18.2 p.214 (SCU2C[8] gate); distilled in
 * qemu-model/peripherals/p2a/DATASHEET-P2A.md.
 *
 *   P2A00 @ MMIOBASE+0xF000  Protection Key: bit0 1=enable / 0=disable back door
 *                            ("When P2A is disabled, it will ignore all the
 *                            P-Bus commands", p.400).  Reset 0.
 *   P2A04 @ MMIOBASE+0xF004  Re-mapping base: bits [31:16] set the high half of
 *                            the target AHB address (p.400).
 *   aperture @ MMIOBASE+0x10000..0x1FFFF: the host's 64KB data window.
 *
 *   AHB address = (P2A04[31:16] << 16) | (host aperture offset[15:0])    (p.400)
 *
 * A host aperture cycle is honoured only when the back door is unlocked
 * (P2A00[0]=1) AND the PCI-slave->AHB bridge is enabled at the SCU (SCU2C[8]=0,
 * read live from the real SCU model over the AHB); otherwise it fails loudly,
 * exactly as an ignored/blocked host cycle raises no AHB command on silicon.
 *
 * --- The honest host-drive back-channel ---
 * The kgpe-d16-bmc machine has no host PCI root complex, so the HOST half of the
 * back door is exposed as QOM properties (the same technique the mainline
 * aspeed_lpc.c / aspeed_lpc_ast2050.c KCS models use for their host LPC ports):
 *
 *   host-p2a00-key    write = host OUT to P2A00 (MMIOBASE+0xF000)  read = P2A00
 *   host-p2a04-remap  write = host OUT to P2A04 (MMIOBASE+0xF004)  read = P2A04
 *   host-p2a-offset   write = latch the aperture offset (low 16b)  read = offset
 *   host-p2a-data     write = host WRITE cycle through the aperture (dword)
 *                     read  = host READ  cycle through the aperture (dword)
 *
 * These properties replace ONLY the physical PCI bus wires (the host memory
 * cycles to BAR1) and the host-BIOS BAR1 placement/memory-space-enable they
 * subsume — the machine cannot have a host CPU. Every effect they trigger is the
 * datasheet P2A behaviour above: the P2A00 unlock, the P2A04 remap equation, the
 * live SCU2C[8] gate, and a genuine AHB dword access that lands on the real
 * modelled peripherals (e.g. the SCU, returning SCU7C=0x202). culvert accesses
 * SoC registers as dwords, so the back-channel models the dword cycle; the
 * translation is access-width independent.
 *
 * This code is licensed under the GPL version 2 or later.
 */
#include "qemu/osdep.h"
#include "hw/misc/aspeed_p2a_ast2050.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define P2A00_ENABLE       0x1u          /* Protection Key bit0 (p.400) */
#define P2A04_REMAP_MASK   0xFFFF0000u   /* Re-mapping base uses [31:16] (p.400) */
#define P2A_APERTURE_MASK  0x0000FFFFu   /* low 16 bits pass through (p.400) */

/*
 * SCU2C[8] "Disable PCI slave to AHB bus bridge" (datasheet §18.2 p.214;
 * 0=enable, 1=disable). SCU @ 0x1E6E2000 in the §9 AHB map (p.97). Read live
 * from the real SCU model over the AHB so the gate reflects the actual BMC/SoC
 * configuration rather than a private copy.
 */
#define ASPEED_P2A_SCU_MISC_CTRL   0x1E6E202Cu
#define SCU2C_PCI_SLAVE_AHB_DIS    0x100u    /* bit8 */

/* Is the PCI-slave->AHB fabric the back door rides on enabled at the SCU? */
static bool aspeed_p2a_scu_bridge_enabled(AspeedP2AAST2050State *s)
{
    MemTxResult res;
    uint32_t scu2c = address_space_ldl_le(&s->ahb_as, ASPEED_P2A_SCU_MISC_CTRL,
                                          MEMTXATTRS_UNSPECIFIED, &res);

    if (res != MEMTX_OK) {
        return false;
    }
    return !(scu2c & SCU2C_PCI_SLAVE_AHB_DIS);   /* bit8=0 -> bridge enabled */
}

/*
 * Validate that a host aperture cycle would be honoured and return the target
 * AHB address. Fails loudly (like an unclaimed host cycle) if the back door is
 * locked or the SCU bridge is disabled.
 */
static bool aspeed_p2a_resolve(AspeedP2AAST2050State *s, hwaddr *ahb,
                               Error **errp)
{
    if (!(s->p2a00 & P2A00_ENABLE)) {
        error_setg(errp, "P2A back door is locked (P2A00 protection key = 0): "
                   "the bridge ignores all P-Bus commands "
                   "(datasheet §36.2 p.400) — write host-p2a00-key = 1 first");
        return false;
    }
    if (!aspeed_p2a_scu_bridge_enabled(s)) {
        error_setg(errp, "PCI-slave->AHB bridge disabled at SCU2C[8]=1 "
                   "(datasheet §18.2 p.214): the host cycle reaches no AHB "
                   "command");
        return false;
    }
    *ahb = (hwaddr)(s->p2a04 & P2A04_REMAP_MASK) |
           (s->host_offset & P2A_APERTURE_MASK);
    return true;
}

/* host-p2a00-key: OUT to P2A00 (bit0 enable; [31:1] reserved, p.400). */
static void aspeed_p2a_set_key(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    AspeedP2AAST2050State *s = ASPEED_P2A_AST2050(obj);
    uint32_t val;

    if (!visit_type_uint32(v, name, &val, errp)) {
        return;
    }
    s->p2a00 = val & P2A00_ENABLE;
}

static void aspeed_p2a_get_key(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    AspeedP2AAST2050State *s = ASPEED_P2A_AST2050(obj);
    uint32_t val = s->p2a00;

    visit_type_uint32(v, name, &val, errp);
}

/* host-p2a04-remap: OUT to P2A04 (bits [31:16] used; [15:0] reserved, p.400). */
static void aspeed_p2a_set_remap(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    AspeedP2AAST2050State *s = ASPEED_P2A_AST2050(obj);
    uint32_t val;

    if (!visit_type_uint32(v, name, &val, errp)) {
        return;
    }
    s->p2a04 = val & P2A04_REMAP_MASK;
}

static void aspeed_p2a_get_remap(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    AspeedP2AAST2050State *s = ASPEED_P2A_AST2050(obj);
    uint32_t val = s->p2a04;

    visit_type_uint32(v, name, &val, errp);
}

/* host-p2a-offset: latch the host's low-16-bit offset into the data aperture. */
static void aspeed_p2a_set_offset(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    AspeedP2AAST2050State *s = ASPEED_P2A_AST2050(obj);
    uint32_t val;

    if (!visit_type_uint32(v, name, &val, errp)) {
        return;
    }
    s->host_offset = val & P2A_APERTURE_MASK;
}

static void aspeed_p2a_get_offset(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    AspeedP2AAST2050State *s = ASPEED_P2A_AST2050(obj);
    uint32_t val = s->host_offset;

    visit_type_uint32(v, name, &val, errp);
}

/* host-p2a-data write: a host WRITE cycle through the aperture -> AHB dword. */
static void aspeed_p2a_set_data(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    AspeedP2AAST2050State *s = ASPEED_P2A_AST2050(obj);
    uint32_t val;
    hwaddr ahb;
    MemTxResult res;

    if (!visit_type_uint32(v, name, &val, errp)) {
        return;
    }
    if (!aspeed_p2a_resolve(s, &ahb, errp)) {
        return;
    }
    address_space_stl_le(&s->ahb_as, ahb, val, MEMTXATTRS_UNSPECIFIED, &res);
    if (res != MEMTX_OK) {
        error_setg(errp, "P2A write to AHB 0x%" HWADDR_PRIx " failed (MemTxResult %d)",
                   ahb, res);
    }
}

/* host-p2a-data read: a host READ cycle through the aperture -> AHB dword. */
static void aspeed_p2a_get_data(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    AspeedP2AAST2050State *s = ASPEED_P2A_AST2050(obj);
    uint32_t val;
    hwaddr ahb;
    MemTxResult res;

    if (!aspeed_p2a_resolve(s, &ahb, errp)) {
        return;
    }
    val = address_space_ldl_le(&s->ahb_as, ahb, MEMTXATTRS_UNSPECIFIED, &res);
    if (res != MEMTX_OK) {
        error_setg(errp, "P2A read from AHB 0x%" HWADDR_PRIx " failed (MemTxResult %d)",
                   ahb, res);
        return;
    }
    visit_type_uint32(v, name, &val, errp);
}

static void aspeed_p2a_ast2050_reset(DeviceState *dev)
{
    AspeedP2AAST2050State *s = ASPEED_P2A_AST2050(dev);

    s->p2a00 = 0;           /* back door locked out of reset (p.400) */
    s->p2a04 = 0;
    s->host_offset = 0;
}

static void aspeed_p2a_ast2050_realize(DeviceState *dev, Error **errp)
{
    AspeedP2AAST2050State *s = ASPEED_P2A_AST2050(dev);

    if (!s->ahb_mr) {
        error_setg(errp, TYPE_ASPEED_P2A_AST2050 ": 'ahb' link not set");
        return;
    }
    address_space_init(&s->ahb_as, s->ahb_mr, "aspeed-p2a-ahb");
}

static void aspeed_p2a_ast2050_init(Object *obj)
{
    object_property_add(obj, "host-p2a00-key", "uint32",
                        aspeed_p2a_get_key, aspeed_p2a_set_key, NULL, NULL);
    object_property_set_description(obj, "host-p2a00-key",
        "Host access to P2A00 protection key (MMIOBASE+0xF000): bit0 unlocks "
        "the PCI->AHB back door");
    object_property_add(obj, "host-p2a04-remap", "uint32",
                        aspeed_p2a_get_remap, aspeed_p2a_set_remap, NULL, NULL);
    object_property_set_description(obj, "host-p2a04-remap",
        "Host access to P2A04 re-mapping base (MMIOBASE+0xF004): bits[31:16] set "
        "the high half of the AHB target address");
    object_property_add(obj, "host-p2a-offset", "uint32",
                        aspeed_p2a_get_offset, aspeed_p2a_set_offset, NULL, NULL);
    object_property_set_description(obj, "host-p2a-offset",
        "Host aperture offset (low 16 bits) for the next data cycle: models the "
        "host addressing MMIOBASE+0x10000+offset");
    object_property_add(obj, "host-p2a-data", "uint32",
                        aspeed_p2a_get_data, aspeed_p2a_set_data, NULL, NULL);
    object_property_set_description(obj, "host-p2a-data",
        "Host data cycle through the P2A aperture: read/write the AHB dword at "
        "(P2A04[31:16]<<16)|offset (needs P2A00[0]=1 and SCU2C[8]=0)");
}

static const VMStateDescription vmstate_aspeed_p2a_ast2050 = {
    .name = TYPE_ASPEED_P2A_AST2050,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(p2a00, AspeedP2AAST2050State),
        VMSTATE_UINT32(p2a04, AspeedP2AAST2050State),
        VMSTATE_UINT32(host_offset, AspeedP2AAST2050State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property aspeed_p2a_ast2050_properties[] = {
    DEFINE_PROP_LINK("ahb", AspeedP2AAST2050State, ahb_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static void aspeed_p2a_ast2050_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = aspeed_p2a_ast2050_realize;
    device_class_set_legacy_reset(dc, aspeed_p2a_ast2050_reset);
    dc->desc = "ASPEED AST2050 P2A (PCI-to-AHB back-door bridge)";
    dc->vmsd = &vmstate_aspeed_p2a_ast2050;
    device_class_set_props(dc, aspeed_p2a_ast2050_properties);
}

static const TypeInfo aspeed_p2a_ast2050_info = {
    .name = TYPE_ASPEED_P2A_AST2050,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AspeedP2AAST2050State),
    .instance_init = aspeed_p2a_ast2050_init,
    .class_init = aspeed_p2a_ast2050_class_init,
};

static void aspeed_p2a_ast2050_register_types(void)
{
    type_register_static(&aspeed_p2a_ast2050_info);
}

type_init(aspeed_p2a_ast2050_register_types)
