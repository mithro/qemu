
#include "qemu/osdep.h"
#include "qemu-common.h"

#include "exec/address-spaces.h"
#include "hw/hw.h"
#include "hw/i2c/smbus.h"
#include "hw/loader.h"
#include "hw/ssi/ssi.h"
#include "hw/sysbus.h"
#include "net/net.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "sysemu/block-backend.h"
#include "sysemu/blockdev.h"
#include "sysemu/sysemu.h"

#include "hw/litex/hw.h"
#include "generated/csr.h"
#include "generated/mem.h"


DeviceState *litex_liteeth_create(hwaddr reg_base, hwaddr phy_base, hwaddr ethmac_sram_base, qemu_irq ethmac_irq)
{
    DeviceState *dev;

    qemu_check_nic_model(&nd_table[0], "liteeth");
    dev = qdev_create(NULL, "litex-liteeth");
    qdev_set_nic_properties(dev, &nd_table[0]);
    qdev_init_nofail(dev);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, phy_base);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1, reg_base);

    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 2, ethmac_sram_base);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, ethmac_irq);
    return dev;
}
