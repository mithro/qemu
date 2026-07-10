/*
 * ASPEED AST2050 (G3) PWM & Tachometer controller.
 *
 * Datasheet §28: 4 PWM outputs + 16 fan-tachometer inputs at 0x1E786000, register
 * window 0x00-0x3C (nothing above). See qemu-model/peripherals/pwm. OpenBMC uses
 * this for fan speed control (hwmon-pwm) and RPM monitoring (hwmon-fan).
 *
 * This code is licensed under the GPL version 2 or later.
 */
#ifndef ASPEED_PWM_AST2050_H
#define ASPEED_PWM_AST2050_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_ASPEED_PWM_AST2050 "aspeed.pwm-ast2050"
OBJECT_DECLARE_SIMPLE_TYPE(AspeedPWMAST2050State, ASPEED_PWM_AST2050)

/* Register file is 0x00-0x3C (PTCR00..PTCR3C); nothing exists at/above 0x40. */
#define ASPEED_PWM_AST2050_NR_REGS (0x40 / 4)

struct AspeedPWMAST2050State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[ASPEED_PWM_AST2050_NR_REGS];
};

#endif /* ASPEED_PWM_AST2050_H */
