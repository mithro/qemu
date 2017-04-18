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
#include "hw/litex/hw.h"
#include "litex.h"
#include "exec/address-spaces.h"
#include "qemu/cutils.h"
#include "hw/char/serial.h"
#include "generated/csr.h"
#include "generated/mem.h"


typedef struct {
    LM32CPU *cpu;
    hwaddr bootstrap_pc;
    hwaddr flash_base;
} ResetInfo;

static void cpu_irq_handler(void *opaque, int irq, int level)
{
    LM32CPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    if (level) {
        cpu_interrupt(cs, CPU_INTERRUPT_HARD);
    } else {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
    }
}

static void main_cpu_reset(void *opaque)
{
    ResetInfo *reset_info = opaque;
    CPULM32State *env = &reset_info->cpu->env;

    cpu_reset(CPU(reset_info->cpu));

    /* init defaults */
    printf("Resetting PC to: 0x%x\n", (unsigned int)reset_info->bootstrap_pc);
    env->pc = reset_info->bootstrap_pc;
    env->eba = ROM_BASE;
    env->deba = ROM_BASE;
}

static void
litex_init(MachineState *machine)
{
    const char *cpu_model = machine->cpu_model;
    const char *kernel_filename = machine->kernel_filename;

    LM32CPU *cpu;
    CPULM32State *env;

    MemoryRegion *address_space_mem = get_system_memory();

    qemu_irq irq[32];
    int i;
    ResetInfo *reset_info;

    reset_info = g_malloc0(sizeof(ResetInfo));

    if (cpu_model == NULL) {
        cpu_model = "lm32-full";
    }
    cpu = cpu_lm32_init(cpu_model);
    if (cpu == NULL) {
        fprintf(stderr, "qemu: unable to find CPU '%s'\n", cpu_model);
        exit(1);
    }

    env = &cpu->env;
    reset_info->cpu = cpu;

    /* create irq lines */
    env->pic_state = litex_pic_init(qemu_allocate_irq(cpu_irq_handler, cpu, 0));
    for (i = 0; i < 32; i++) {
        irq[i] = qdev_get_gpio_in(env->pic_state, i);
    }

    litex_create_memory(address_space_mem, irq);

    /** addresses from 0x80000000 to 0xFFFFFFFF are not shadowed */
    cpu_lm32_set_phys_msb_ignore(env, 1);

    /* make sure juart isn't the first chardev */
    env->juart_state = lm32_juart_init(serial_hds[1]);

    if (kernel_filename) {
        uint64_t entry;
        int kernel_size;

        /* Boots a kernel elf binary.  */
        kernel_size = load_elf(kernel_filename, NULL, NULL, &entry, NULL, NULL, 1, EM_LATTICEMICO32, 0, 0);
        if (kernel_size < 0) {
            fprintf(stderr, "qemu: could not load kernel '%s'\n",    kernel_filename);
            exit(1);
        }
        reset_info->bootstrap_pc = entry;
    } else {
        reset_info->bootstrap_pc = CONFIG_CPU_RESET_ADDR;
    }
    qemu_register_reset(main_cpu_reset, reset_info);
}

static void litex_machine_init(MachineClass *mc)
{
    mc->desc = "Litex One";
    mc->init = litex_init;
    mc->is_default = 0;
}

DEFINE_MACHINE("litex", litex_machine_init)
