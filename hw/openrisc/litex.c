/*
 *  QEMU model for the Litex board.
 *
 *  Copyright (c) 2010 Michael Walle <michael@walle.cc>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "assert.h"

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "cpu.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "sysemu/sysemu.h"
#include "sysemu/qtest.h"
#include "hw/devices.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "elf.h"
#include "sysemu/block-backend.h"
#include "exec/address-spaces.h"
#include "qemu/cutils.h"
#include "hw/char/serial.h"

#include "hw/litex/hw.h"
#include "generated/csr.h"
#include "generated/mem.h"


typedef struct {
    OpenRISCCPU *cpu;
    hwaddr bootstrap_pc;
    hwaddr flash_base;
} ResetInfo;


static void main_cpu_reset(void *opaque)
{
    ResetInfo *reset_info = opaque;
    OpenRISCCPU *cpu = reset_info->cpu;

    cpu_reset(CPU(cpu));
    printf("Resetting PC to: 0x%x\n", (unsigned int)reset_info->bootstrap_pc);
    cpu->env.pc = reset_info->bootstrap_pc;
}

static void
litex_init(MachineState *machine)
{
    const char *cpu_model = machine->cpu_model;
    const char *kernel_filename = machine->kernel_filename;

    OpenRISCCPU *cpu;
    MemoryRegion *address_space_mem = get_system_memory();

    ResetInfo *reset_info;
    reset_info = g_malloc0(sizeof(ResetInfo));

    if (cpu_model == NULL) {
        cpu_model = "or1200";
    }

    cpu = cpu_openrisc_init(cpu_model);
    if (cpu == NULL) {
        fprintf(stderr, "qemu: unable to find CPU '%s'\n", cpu_model);
        exit(1);
    }
    qemu_register_reset(main_cpu_reset, reset_info);

    reset_info->cpu = cpu;

    //cpu_lm32_set_phys_msb_ignore(cpu->env, 1);
    cpu_openrisc_pic_init(cpu);
    cpu_openrisc_clock_init(cpu);

    litex_create_memory(address_space_mem, (qemu_irq*)(cpu->env.irq));

    /* make sure juart isn't the first chardev */
    //cpu->env->juart_state = lm32_juart_init(serial_hds[1]);

    if (kernel_filename) {
        uint64_t entry;
        int kernel_size;

        /* Boots a kernel elf binary.  */
        kernel_size = load_elf(kernel_filename, NULL, NULL, &entry, NULL, NULL, 1, EM_OPENRISC, 0, 0);
        reset_info->bootstrap_pc = entry;

        if (kernel_size < 0) {
            fprintf(stderr, "qemu: could not load kernel '%s'\n",    kernel_filename);
            exit(1);
        }
    } else {
        reset_info->bootstrap_pc = CONFIG_CPU_RESET_ADDR;
    }
    reset_info->bootstrap_pc = 0x40000000+0x100;
}

static void litex_machine_init(MachineClass *mc)
{
    mc->desc = "Litex One";
    mc->init = litex_init;
    mc->is_default = 0;
}

DEFINE_MACHINE("litex", litex_machine_init)
