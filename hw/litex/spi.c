
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

#define MEM_SIZE 0x80000000
#define MEM_MASK 0x7FFFFFFF

void* m25p80_get_storage(void *opaque);

void litex_create_memory(MemoryRegion *address_space_mem, qemu_irq irqs[])
{

#ifdef SPIFLASH_BASE
    {
        MemoryRegion *phys_spiflash = g_new(MemoryRegion, 1);
        hwaddr spiflash_base = SPIFLASH_BASE;
        size_t spiflash_size = SPIFLASH_SIZE;
        void* spiflash_data;

        DriveInfo *dinfo = drive_get_next(IF_MTD);

#ifdef CSR_SPIFLASH_BASE
        DeviceState *spi_master;
        DeviceState *spi_flash;
        SSIBus *spi_bus;
        qemu_irq cs_line;

        spi_master = qdev_create(NULL, "litex_ssi");
        qdev_init_nofail(spi_master);
        sysbus_mmio_map(SYS_BUS_DEVICE(spi_master), 0, CSR_SPIFLASH_BASE & MEM_MASK);

        spi_bus = (SSIBus *)qdev_get_child_bus(spi_master, "ssi");

        if (dinfo) {
            if (!dinfo->serial) {
                printf("Set serial value to flash type (m25p16, etc)\n");
                abort();
            } else {
                printf("Using spiflash type %s\n", dinfo->serial);
            }
            spi_flash = ssi_create_slave_no_init(spi_bus, dinfo->serial);
            qdev_prop_set_drive(spi_flash, "drive", blk_by_legacy_dinfo(dinfo), &error_abort);
        } else {
            spi_flash = ssi_create_slave_no_init(spi_bus, "m25p80");
        }
        qdev_init_nofail(spi_flash);

        cs_line = qdev_get_gpio_in_named(spi_flash, SSI_GPIO_CS, 0);
        qdev_connect_gpio_out_named(spi_master, SSI_GPIO_CS, 0, cs_line);

        spiflash_data = m25p80_get_storage(spi_flash);
#else
        if (dinfo) {
            int rsize = 0;
            BlockBackend *blk = blk_by_legacy_dinfo(dinfo);
            assert(blk);

            spiflash_data = blk_blockalign(blk, spiflash_size);

            rsize = blk_pread(blk, 0, spiflash_data, spiflash_size);
            if (rsize != spiflash_size) {
                printf("litex.spiflash: Failed to read flash contents, wanted s->size %d, got %d\n", (int)spiflash_size, rsize);
                abort();
            }
        }
#endif

        if (spiflash_data) {
            memory_region_init_ram_device_ptr(phys_spiflash, NULL, "litex.spiflash", spiflash_size, spiflash_data);
        } else {
            memory_region_allocate_system_memory(phys_spiflash, NULL, "litex.spiflash", spiflash_size);
        }
        memory_region_set_readonly(phys_spiflash, true);
        memory_region_add_subregion(address_space_mem, spiflash_base, phys_spiflash);
    }
#endif

}
