/*
 * LiteX Bit-Banging I2C controller
 *
 * Copyright (c) 2006-2007 CodeSourcery.
 * Copyright (c) 2012 Oskar Andero <oskar.andero@gmail.com>
 *
 * This file is derived from hw/realview.c by Paul Brook
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "bitbang_i2c.h"
#include "qemu/log.h"

#define TYPE_LITEX_I2C "litex_i2c"
#define LITEX_I2C(obj) \
    OBJECT_CHECK(LiteXI2CState, (obj), TYPE_LITEX_I2C)

enum {
    R_OPSIS_I2C_MASTER_W,
    R_OPSIS_I2C_MASTER_R,
    R_MAX,
};

#define I2C_SCL         0x01
#define I2C_SDAOE	0x02
#define I2C_SDAOUT	0x04

#define I2C_SDAIN	0x01

typedef struct LiteXI2CState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    bitbang_i2c_interface *bitbang;
    uint32_t regs[R_MAX];

} LiteXI2CState;

static uint64_t litex_i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    LiteXI2CState *s = (LiteXI2CState *)opaque;
    addr >>= 2;
    if (addr >= R_MAX) {
	printf("Tried to access invalid address %08x\n", (unsigned int)addr);
	return 0;
    }

    uint64_t r = s->regs[addr];
    return r;
}

static void litex_i2c_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    int scl, sda_oe, sda_out, sda_in;
    LiteXI2CState *s = (LiteXI2CState *)opaque;
    addr >>= 2;

    if (addr >= R_MAX) {
	printf("Tried to access invalid address %08x\n", (unsigned int)addr);
	return;
    }

    s->regs[addr] = value & 0xff;
    switch(addr)
    {
    case R_OPSIS_I2C_MASTER_W:
	scl = (value & I2C_SCL) ? 1 : 0;
	sda_oe = (s->regs[addr] & I2C_SDAOE) ? 1 : 0;
	sda_out = (s->regs[addr] & I2C_SDAOUT) ? 1 : 0;

	bitbang_i2c_set(s->bitbang, BITBANG_I2C_SCL, scl);
	sda_in = bitbang_i2c_set(s->bitbang, BITBANG_I2C_SDA, sda_oe ? sda_out : 1);
	if (!sda_oe) {
		s->regs[R_OPSIS_I2C_MASTER_R] = sda_in; // FIXME: Shift?
	}
    }
}

static const MemoryRegionOps litex_i2c_ops = {
    .read = litex_i2c_read,
    .write = litex_i2c_write,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void litex_i2c_init(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    LiteXI2CState *s = LITEX_I2C(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    I2CBus *bus;

    bus = i2c_init_bus(dev, "i2c");
    s->bitbang = bitbang_i2c_init(bus);
    memory_region_init_io(&s->iomem, OBJECT(dev), &litex_i2c_ops, s, "litex_i2c", R_MAX * 4);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const TypeInfo litex_i2c_info = {
    .name          = TYPE_LITEX_I2C,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(LiteXI2CState),
    .instance_init = litex_i2c_init,
};

static void litex_i2c_register_types(void)
{
    type_register_static(&litex_i2c_info);
}

type_init(litex_i2c_register_types)
