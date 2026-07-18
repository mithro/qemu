/*
 * ASPEED AST2050 (G3) USB2.0 device / virtual-hub controller.
 *
 * Base 0x1E6A0000 (datasheet): HUB00 root control + device blocks + a 21-endpoint
 * pool with DMA descriptors. This is the BMC virtual-media / virtual-HID path
 * (OpenBMC obmc-ikvm). The AST2050 has NO EHCI host controller — all USB goes
 * through this device/vhub. See qemu-model/peripherals/usb.
 *
 * This code is licensed under the GPL version 2 or later.
 */
#ifndef ASPEED_UDC_AST2050_H
#define ASPEED_UDC_AST2050_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_ASPEED_UDC_AST2050 "aspeed.udc-ast2050"
OBJECT_DECLARE_SIMPLE_TYPE(AspeedUDCAST2050State, ASPEED_UDC_AST2050)

/*
 * Device-controller register window: HUB00 root ctrl + device blocks + the full
 * 21-endpoint pool. The pool's EPP#16-#20 live at 0x300-0x34F (datasheet Sec
 * 15.3.1 p.155), so the window must reach 0x350 -- the mainline driver computes
 * per-EP register addresses at base + 0x200 + i*0x10 and would fault past 0x300
 * for the 17th+ generic endpoint.
 */
#define ASPEED_UDC_AST2050_NR_REGS (0x350 / 4)

struct AspeedUDCAST2050State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;

    /*
     * G3 PHY/deadlock semantics (datasheet Sec 15.3.2): CTRL[31] PHY-clock is a
     * READ-ONLY ready status that comes up only once the driver polls it after
     * releasing PHY reset; asserting UPSTREAM_CONNECT into a not-ready PHY (a
     * driver that connects without polling) latches the fatal, level-triggered
     * "USB command bus dead-lock" (ISR[18]).
     */
    bool phy_ready;
    bool deadlocked;

    /*
     * "deadlock-model" property (default OFF). The bus-dead-lock hazard is REAL on
     * silicon, but the only deterministic signal QEMU (no icount) has to tell the
     * mainline driver (connects blind -> hangs on silicon) apart from a driver that
     * is safe on silicon is "did it read CTRL[31] to poll". That proxy is WRONG for
     * the Avocent/C410X vendor firmware, which is safe on real silicon yet does NOT
     * poll CTRL[31] (traced 2026-07-18): it releases reset, sets up EPs and then
     * connects, exactly like the *unpatched* mainline driver bar the poll's
     * udelay() timing -- which QEMU cannot represent. So the model cannot latch the
     * deadlock for the mainline driver without ALSO false-latching it for the
     * vendor firmware (breaking the C4 legacy boot). Keep the hazard modelling
     * available for the kernel-patch-0007 repro, but OFF by default so legacy
     * firmware always boots (the primary faithfulness rule). See LOG.md 2026-07-18.
     */
    bool deadlock_model;

    uint32_t regs[ASPEED_UDC_AST2050_NR_REGS];
};

#endif /* ASPEED_UDC_AST2050_H */
