/*
 * Digi NS9360 (NET+ARM) Machine Emulation
 *
 * This models the NS9360 SoC as used in the HPE iPDU (Intelligent Power
 * Distribution Unit).  The NS9360 is based on the ARM926EJ-S core.
 *
 * Copyright (c) 2025 The QEMU contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/boards.h"
#include "hw/block/flash.h"
#include "hw/qdev-properties.h"
#include "hw/core/cpu.h"
#include "exec/address-spaces.h"
#include "system/system.h"
#include "system/blockdev.h"
#include "qom/object.h"
#include "target/arm/cpu-qom.h"

/*
 * NS9360 SoC memory map
 *
 * 0x00000000  SDRAM (32 MB)
 * 0x40000000  CS0 - NOR flash (8 MB) - boot device
 * 0x50000000  CS1 - NOR flash (8 MB)
 * 0x90200040  UART Port A (serial console)
 * 0x90600000  BBus bridge (GPIO, misc)
 * 0xA0700000  Memory controller registers
 * 0xA0900000  System control registers (PLL, clocks)
 */

#define NS9360_SDRAM_BASE       0x00000000
#define NS9360_SDRAM_SIZE       (32 * MiB)

#define NS9360_FLASH0_BASE      0x40000000  /* CS0 boot flash */
#define NS9360_FLASH1_BASE      0x50000000  /* CS1 flash */
#define NS9360_FLASH_SIZE       (8 * MiB)
#define NS9360_FLASH_SECTOR     (64 * KiB)

#define NS9360_UART_A_BASE      0x90200040

#define NS9360_BBUS_BASE        0x90600000
#define NS9360_MEMCTRL_BASE     0xA0700000
#define NS9360_SYS_BASE         0xA0900000

#define TYPE_NS9360_UART    "ns9360-uart"
#define TYPE_NS9360_SYSREGS "ns9360-sysregs"

static void ns9360_init(MachineState *machine)
{
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    MemoryRegion *sysmem = get_system_memory();
    ARMCPU *cpu;
    DeviceState *uart_dev;
    SysBusDevice *uart_sbd;
    DeviceState *sysregs_dev;
    SysBusDevice *sysregs_sbd;
    Chardev *chr;

    /* Validate RAM size */
    if (machine->ram_size != mc->default_ram_size) {
        char *sz = size_to_str(mc->default_ram_size);
        error_report("Invalid RAM size, should be %s", sz);
        g_free(sz);
        exit(EXIT_FAILURE);
    }

    /* Create CPU */
    cpu = ARM_CPU(cpu_create(machine->cpu_type));

    /* Map SDRAM */
    memory_region_add_subregion(sysmem, NS9360_SDRAM_BASE, machine->ram);

    /*
     * NOR flash at CS0 (boot device) and CS1.
     *
     * Intel/Micron 28F640J3 or similar CFI-compliant NOR flash.
     * Using CFI type 01 (Intel command set).
     * Width = 2 bytes (16-bit data bus).
     */
    for (unsigned i = 0; i < 2; i++) {
        DriveInfo *dinfo = drive_get(IF_PFLASH, 0, i);
        pflash_cfi01_register(
            i ? NS9360_FLASH1_BASE : NS9360_FLASH0_BASE,
            i ? "ns9360.flash1" : "ns9360.flash0",
            NS9360_FLASH_SIZE,
            dinfo ? blk_by_legacy_dinfo(dinfo) : NULL,
            NS9360_FLASH_SECTOR,
            2,             /* width: 16-bit */
            0x0089,        /* manufacturer: Intel */
            0x0017,        /* device: 28F640J3 (64 Mbit) */
            0x0000, 0x0000,
            0              /* little-endian */
        );
    }

    /*
     * NS9360 UART Port A
     *
     * Create the UART device and connect it to the first serial backend.
     */
    uart_dev = qdev_new(TYPE_NS9360_UART);
    chr = serial_hd(0);
    if (chr) {
        qdev_prop_set_chr(uart_dev, "chardev", chr);
    }
    uart_sbd = SYS_BUS_DEVICE(uart_dev);
    sysbus_realize_and_unref(uart_sbd, &error_fatal);
    sysbus_mmio_map(uart_sbd, 0, NS9360_UART_A_BASE);

    /*
     * System registers, memory controller, and BBus stubs.
     *
     * This single device provides three MMIO regions:
     *   region 0 -> SYS  at 0xA0900000 (256 KB)
     *   region 1 -> MEM  at 0xA0700000 (256 KB)
     *   region 2 -> BBUS at 0x90600000 (256 KB)
     */
    sysregs_dev = qdev_new(TYPE_NS9360_SYSREGS);
    sysregs_sbd = SYS_BUS_DEVICE(sysregs_dev);
    sysbus_realize_and_unref(sysregs_sbd, &error_fatal);
    sysbus_mmio_map(sysregs_sbd, 0, NS9360_SYS_BASE);
    sysbus_mmio_map(sysregs_sbd, 1, NS9360_MEMCTRL_BASE);
    sysbus_mmio_map(sysregs_sbd, 2, NS9360_BBUS_BASE);

    /*
     * Set CPU entry point.
     *
     * The NS9360 boots from CS0 (NOR flash at 0x40000000).  The ARM926EJ-S
     * fetches its reset vector from address 0x00000000 by default, but on
     * the NS9360 the boot ROM / hardware maps CS0 to the reset vector
     * region.  We set the PC directly to the flash base so that execution
     * begins from the flash image.
     */
    cpu_set_pc(CPU(cpu), NS9360_FLASH0_BASE);
}

static void ns9360_machine_init(MachineClass *mc)
{
    mc->desc = "Digi NS9360 / NET+ARM (ARM926EJ-S)";
    mc->init = ns9360_init;
    mc->ignore_memory_transaction_failures = true;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm926");
    mc->default_ram_size = NS9360_SDRAM_SIZE;
    mc->default_ram_id = "ns9360.sdram";
}

DEFINE_MACHINE("ns9360", ns9360_machine_init)
