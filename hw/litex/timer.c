
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

DeviceState *litex_timer_create(hwaddr base, qemu_irq timer0_irq, uint32_t freq_hz)
{
    DeviceState *dev;

    dev = qdev_create(NULL, "litex-timer");
    qdev_prop_set_uint32(dev, "frequency", freq_hz);
    qdev_init_nofail(dev);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, timer0_irq);

    return dev;
}
