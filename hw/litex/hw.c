
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

void litex_create_memory(MemoryRegion *address_space_mem, qemu_irq irqs[])
{
    /*
      The following two memory regions equivalent to each other;
       (a) 0x00000000 to 0x7FFFFFFF
       (b) 0x80000000 to 0xFFFFFFFF

      IE Memory found at 0x00000100 will also be found at 0x80000100.

      On a real system accessing the memory via (a) goes through the CPU cache,
      while accessing it via (b) bypasses the cache.

      However, as QEmu doesn't emulate the CPU cache, we can just alias them
      together.
    */
    MemoryRegion *shadow_mem = g_new(MemoryRegion, 1);
    memory_region_init_alias(shadow_mem, NULL, "litex.shadow", address_space_mem, 0, MEM_SIZE);
    memory_region_add_subregion(address_space_mem, MEM_SIZE, shadow_mem);

/*
    {
        MemoryRegion *phys_zero = g_new(MemoryRegion, 1);
        memory_region_allocate_system_memory(phys_zero, NULL, "litex.zero", 0x100);
        memory_region_add_subregion(address_space_mem, 0, phys_zero);
    }
*/

    {
        char *bios_filename = NULL;
        bios_filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);

#ifdef ROM_BASE
#ifndef ROM_DISABLE
        {
            MemoryRegion *phys_rom = g_new(MemoryRegion, 1);
            hwaddr rom_base   = ROM_BASE;
            size_t rom_size   = ROM_SIZE;
            memory_region_allocate_system_memory(phys_rom, NULL, "litex.rom", rom_size);
            memory_region_add_subregion(address_space_mem, rom_base, phys_rom);
        }

        /* Load bios rom. */
        if (bios_filename) {
            int bios_size = load_image_targphys(bios_filename, ROM_BASE, ROM_SIZE);
            if (bios_size < 0) {
                fprintf(stderr, "qemu: could not load bios '%s'\n", bios_filename);
                exit(1);
            }
        }
#else
        /* Complain if bios provided. */
        if (bios_filename) {
            fprintf(stderr, "qemu: bios '%s' provided but device has no bios rom!\n", bios_filename);
            exit(1);
        }
#endif
#endif
        g_free(bios_filename);
    }

#ifdef SRAM_BASE
    {
        MemoryRegion *phys_sram = g_new(MemoryRegion, 1);
        hwaddr sram_base   = SRAM_BASE;
        size_t sram_size   = SRAM_SIZE;
        memory_region_allocate_system_memory(phys_sram, NULL, "litex.sram", sram_size);
        memory_region_add_subregion(address_space_mem, sram_base, phys_sram);
    }
#endif

#ifdef MAIN_RAM_BASE
    {
        MemoryRegion *phys_main_ram = g_new(MemoryRegion, 1);
        hwaddr main_ram_base   = MAIN_RAM_BASE;
        size_t main_ram_size   = MAIN_RAM_SIZE;
        memory_region_allocate_system_memory(phys_main_ram, NULL, "litex.main_ram", main_ram_size);
        memory_region_add_subregion(address_space_mem, main_ram_base, phys_main_ram);
    }
#endif


#ifdef CSR_OPSIS_I2C_BASE
    litex_i2c_create(CSR_OPSIS_I2C_BASE & MEM_MASK);
#endif

/* litex ethernet*/
#ifdef CSR_ETHMAC_BASE
    litex_liteeth_create(CSR_ETHMAC_BASE & MEM_MASK, CSR_ETHPHY_BASE & MEM_MASK, ETHMAC_BASE & MEM_MASK, irqs[ETHMAC_INTERRUPT]);
#endif

}
