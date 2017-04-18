
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

DeviceState *litex_uart_create(hwaddr base, qemu_irq irq, Chardev *chr)
{
    DeviceState *dev;

    dev = qdev_create(NULL, "litex-uart");
    qdev_prop_set_chr(dev, "chardev", chr);
    qdev_init_nofail(dev);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, irq);

    return dev;
}

#define MEM_SIZE 0x80000000
#define MEM_MASK 0x7FFFFFFF

void litex_create_memory(MemoryRegion *address_space_mem, qemu_irq irqs[])
{
    /* litex uart */
#ifdef CSR_UART_BASE
    litex_uart_create(CSR_UART_BASE & MEM_MASK, irqs[2 /*UART_INTERRUPT*/], serial_hds[0]);
#endif

}
