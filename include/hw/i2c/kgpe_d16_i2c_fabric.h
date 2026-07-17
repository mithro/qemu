/*
 * ASUS KGPE-D16 I2C mux fabric (QU9 + QU5 + U23) behind BMC bus I2C2.
 * See hw/i2c/kgpe_d16_i2c_fabric.c and
 * asus-kgpe-d16-firmware/schematic-wiring/I2C-MUX-FABRIC-ARBITRATION.md.
 *
 * Copyright 2026, Apache-2.0 OR GPL-2.0-or-later
 */
#ifndef HW_I2C_KGPE_D16_I2C_FABRIC_H
#define HW_I2C_KGPE_D16_I2C_FABRIC_H

#include "hw/i2c/i2c.h"

#define TYPE_KGPE_D16_I2C_FABRIC "kgpe-d16-i2c-fabric"

/* Channels, matching QU5: Y0 aux-panel/TPM/PCIe, Y1 n/c, Y2 DIMM A-D,
 * Y3 DIMM E-H. */
#define KGPE_D16_FABRIC_Y0_AUXPANEL 0
#define KGPE_D16_FABRIC_Y1_NC       1
#define KGPE_D16_FABRIC_Y2_DIMM_AD  2
#define KGPE_D16_FABRIC_Y3_DIMM_EH  3

I2CBus *kgpe_d16_i2c_fabric_get_bus(DeviceState *dev, uint8_t channel);

#endif
