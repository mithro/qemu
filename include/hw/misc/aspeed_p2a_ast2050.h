/*
 * ASPEED AST2050 (G3) P2A — PCI(P-Bus)-to-AHB back-door bridge.
 *
 * The culvert `p2a` back door (validated on the real AST2050: SCU7C=0x202 read
 * over P2A). A host PCI master reaches the BMC's internal AHB address space
 * through the PCI-slave BAR1 (MMIOBASE) memory window:
 *
 *   P2A00 @ MMIOBASE+0xF000  Protection Key (bit0: 0=disable, 1=enable back door)
 *   P2A04 @ MMIOBASE+0xF004  Re-mapping base address (bits [31:16])
 *   data aperture @ MMIOBASE+0x10000..0x1FFFF (second 64KB of BAR1)
 *
 *   AHB address = (P2A04[31:16] << 16) | (host aperture offset[15:0])
 *
 * gated by P2A00[0]=1 (host unlock) AND SCU2C[8]=0 (PCI-slave->AHB bridge
 * enable). See AST2050/AST1100 A3 datasheet V1.05 §36 p.400 (P2A), §33 p.363-368
 * (PCI slave / BAR1), §18.2 p.214 (SCU2C[8]); distilled in
 * qemu-model/peripherals/p2a/DATASHEET-P2A.md.
 *
 * The kgpe-d16-bmc machine has no host PCI root complex, so the HOST half of the
 * back door (the PCI memory cycles to BAR1, plus the host-BIOS BAR1 placement it
 * subsumes) is driven through QOM properties (see aspeed_p2a_ast2050.c). The
 * translation, the P2A00/P2A04 semantics and the SCU2C[8] gate are the modelled
 * silicon behaviour; the AHB access lands on the real modelled peripherals.
 *
 * This code is licensed under the GPL version 2 or later.
 */
#ifndef ASPEED_P2A_AST2050_H
#define ASPEED_P2A_AST2050_H

#include "hw/sysbus.h"
#include "qom/object.h"
#include "exec/memory.h"

#define TYPE_ASPEED_P2A_AST2050 "aspeed.p2a-ast2050"
OBJECT_DECLARE_SIMPLE_TYPE(AspeedP2AAST2050State, ASPEED_P2A_AST2050)

struct AspeedP2AAST2050State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion *ahb_mr;   /* the SoC AHB memory the back door masters (link) */
    AddressSpace ahb_as;

    uint32_t p2a00;         /* Protection Key   (bit0 enable; reset 0) */
    uint32_t p2a04;         /* Re-mapping base  (bits [31:16]) */
    uint32_t host_offset;   /* current aperture offset (low 16 bits) latch */
};

#endif /* ASPEED_P2A_AST2050_H */
