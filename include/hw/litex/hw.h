#ifndef QEMU_HW_LITEX_HW_H
#define QEMU_HW_LITEX_HW_H

#include "exec/address-spaces.h"
#include "hw/i2c/smbus.h"
#include "hw/boards.h"
#include "hw/qdev.h"
#include "net/net.h"

DeviceState *litex_uart_create(hwaddr base, qemu_irq irq, Chardev *chr);
DeviceState *litex_timer_create(hwaddr base, qemu_irq timer0_irq, uint32_t freq_hz);
DeviceState *litex_liteeth_create(hwaddr reg_base, hwaddr phy_base, hwaddr ethmac_sram_base, qemu_irq ethmac_irq);
DeviceState *litex_i2c_create(hwaddr reg_base);

void litex_create_memory(MemoryRegion *address_space_mem, qemu_irq irqs[]);

#endif /* QEMU_HW_LITEX_HW_H */
