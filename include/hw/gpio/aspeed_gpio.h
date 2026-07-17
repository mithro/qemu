/*
 *  ASPEED GPIO Controller
 *
 *  Copyright (C) 2017-2018 IBM Corp.
 *
 * This code is licensed under the GPL version 2 or later.  See
 * the COPYING file in the top-level directory.
 */

#ifndef ASPEED_GPIO_H
#define ASPEED_GPIO_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_ASPEED_GPIO "aspeed.gpio"
OBJECT_DECLARE_TYPE(AspeedGPIOState, AspeedGPIOClass, ASPEED_GPIO)

#define ASPEED_GPIO_MAX_NR_SETS 8
#define ASPEED_GPIOS_PER_SET 32
#define ASPEED_REGS_PER_BANK 14
#define ASPEED_GPIO_MAX_NR_REGS (ASPEED_REGS_PER_BANK * ASPEED_GPIO_MAX_NR_SETS)
#define ASPEED_GROUPS_PER_SET 4
#define ASPEED_GPIO_NR_DEBOUNCE_REGS 3
#define ASPEED_CHARS_PER_GROUP_LABEL 4

typedef struct GPIOSets GPIOSets;

typedef struct GPIOSetProperties {
    uint32_t input;
    uint32_t output;
    char group_label[ASPEED_GROUPS_PER_SET][ASPEED_CHARS_PER_GROUP_LABEL];
} GPIOSetProperties;

enum GPIORegType {
    gpio_not_a_reg,
    gpio_reg_data_value,
    gpio_reg_direction,
    gpio_reg_int_enable,
    gpio_reg_int_sens_0,
    gpio_reg_int_sens_1,
    gpio_reg_int_sens_2,
    gpio_reg_int_status,
    gpio_reg_reset_tolerant,
    gpio_reg_debounce_1,
    gpio_reg_debounce_2,
    gpio_reg_cmd_source_0,
    gpio_reg_cmd_source_1,
    gpio_reg_data_read,
    gpio_reg_input_mask,
};

/* GPIO index mode */
enum GPIORegIndexType {
    gpio_reg_idx_data = 0,
    gpio_reg_idx_direction,
    gpio_reg_idx_interrupt,
    gpio_reg_idx_debounce,
    gpio_reg_idx_tolerance,
    gpio_reg_idx_cmd_src,
    gpio_reg_idx_input_mask,
    gpio_reg_idx_reserved,
    gpio_reg_idx_new_w_cmd_src,
    gpio_reg_idx_new_r_cmd_src,
};

typedef struct AspeedGPIOReg {
    uint16_t set_idx;
    enum GPIORegType type;
} AspeedGPIOReg;

struct AspeedGPIOClass {
    SysBusDevice parent_obj;
    const GPIOSetProperties *props;
    uint32_t nr_gpio_pins;
    uint32_t nr_gpio_sets;
    const AspeedGPIOReg *reg_table;
    unsigned reg_table_count;
    uint64_t mem_size;
    const MemoryRegionOps *reg_ops;
};

struct AspeedGPIOState {
    /* <private> */
    SysBusDevice parent;

    /*< public >*/
    MemoryRegion iomem;
    int pending;
    qemu_irq irq;
    qemu_irq gpios[ASPEED_GPIO_MAX_NR_SETS][ASPEED_GPIOS_PER_SET];

    /*
     * ASUS KGPE-D16 (AST2050) board power-sequencer glue. Off by default; the
     * kgpe-d16-bmc machine enables it (qdev property "kgpe-d16-pwrseq") so the
     * OpenBMC host-power path is observable/testable in emulation. The BMC's
     * three active-low request lines (GPIOB1 power-up, GPIOF0 power-down,
     * GPIOB6 reset) drive a modeled host-power latch that feeds back on the
     * GPIOH2 power-state input. See hw/gpio/aspeed_gpio.c and
     * asus-kgpe-d16-firmware/openbmc/bmc-functionality/HW-WIRING-power-sensors.md.
     * Every other Aspeed machine leaves kgpe_d16_pwrseq false and is unaffected.
     */
    bool kgpe_d16_pwrseq;       /* qdev property: enable the board glue */
    bool kgpe_d16_host_on;      /* modeled host-power latch (GPIOH2 reflects it) */
    bool kgpe_d16_pwrseq_busy;  /* re-entrancy guard while driving GPIOH2 */

    /*
     * KGPE-D16 board-glue named outputs (only wired when kgpe_d16_pwrseq):
     * "kgpe-host-on"  - the host-power latch level (SYS_PWRGD equivalent),
     *                   consumed by the I2C mux fabric (QU9 gate).
     * "kgpe-i2cs"[2]  - the AST_I2CS0/1 select nets (GPIOF4/F5 through their
     *                   4.7k pull-ups: an undriven pin reads high).
     */
    qemu_irq kgpe_host_on_out;
    qemu_irq kgpe_i2cs_out[2];

    /* Parallel GPIO Registers */
    uint32_t debounce_regs[ASPEED_GPIO_NR_DEBOUNCE_REGS];
    struct GPIOSets {
        uint32_t data_value; /* Reflects pin values */
        uint32_t data_read; /* Contains last value written to data value */
        uint32_t direction;
        uint32_t int_enable;
        uint32_t int_sens_0;
        uint32_t int_sens_1;
        uint32_t int_sens_2;
        uint32_t int_status;
        uint32_t reset_tol;
        uint32_t cmd_source_0;
        uint32_t cmd_source_1;
        uint32_t debounce_1;
        uint32_t debounce_2;
        uint32_t input_mask;
    } sets[ASPEED_GPIO_MAX_NR_SETS];
};

#endif /* ASPEED_GPIO_H */
