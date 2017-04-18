/*
 * Bit-Bang ssi emulation extracted from
 * Marvell MV88W8618 / Freecom MusicPal emulation.
 *
 * Copyright (c) 2008 Jan Kiszka
 *
 * This code is licensed under the GNU GPL v2.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */
#include "qemu/osdep.h"
#include "hw/hw.h"
#include "bitbang_ssi.h"
#include "hw/sysbus.h"

//#define DEBUG_BITBANG_SSI

#ifdef DEBUG_BITBANG_SSI
#define DPRINTF(fmt, ...) \
do { printf("bitbang_ssi: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) do {} while(0)
#endif

typedef enum {
    ACTIVE = 0,
    IDLE
} bitbang_ssi_state;

typedef enum {
    ACTIVE2IDLE = 0,
    IDLE2ACTIVE
} bitbang_ssi_sclk_state;

struct bitbang_ssi_interface {
    SSIBus *bus;

    // Configuration
    int transfer_size;
    bitbang_ssi_cpol_mode cpol_mode;
    bitbang_ssi_cpha_mode cpha_mode;

    // Pin state
    int last_sclk;
    int last_mosi;
    int last_miso;

    // Data transmission state
    bitbang_ssi_state state;
    uint32_t data_miso;
    int data_miso_count;
    uint32_t data_mosi;
    int data_mosi_count;

};

static unsigned char reverse_bits(unsigned char b) {
   b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
   b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
   b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
   return b;
}

static void bitbang_ssi_get_miso(bitbang_ssi_interface *ssi)
{
    if (ssi->data_miso_count > 0) {
        ssi->last_miso = ssi->data_miso & 0x1;
        ssi->data_miso = ssi->data_miso >> 1;
        ssi->data_miso_count -= 1;
    }
#ifdef DEBUG_BITBANG_SSI
    {
    	int i = 0;
        DPRINTF("get_miso %x (%d)[", ssi->last_miso, ssi->data_miso_count);
        for (i=0; i < ssi->data_miso_count; i++) {
            printf("%s", ((ssi->data_miso >> i) & 0x1) ? "1" : "0");
        }
        printf("]\n");
    }
#endif
}

static void bitbang_ssi_set_mosi(bitbang_ssi_interface *ssi)
{
    ssi->data_mosi = ssi->data_mosi << 1 | ssi->last_mosi;
    ssi->data_mosi_count += 1;
#ifdef DEBUG_BITBANG_SSI
    {
        int i = 0;
        DPRINTF("set_mosi %x (%d)[", ssi->last_mosi, ssi->data_mosi_count);
        for (i=0; i < ssi->data_mosi_count; i++) {
            printf("%s", ((ssi->data_mosi >> i) & 0x1) ? "1" : "0");
        }
        printf("]\n");
    }
#endif
}

/* Returns MISO line level or -1. */
int bitbang_ssi_set(bitbang_ssi_interface *ssi, bitbang_ssi_line line, int level)
{
    if (level != 0 && level != 1) {
        abort();
    }

    switch(line) {
    case BITBANG_SSI_SCLK:
        if (ssi->last_sclk == level)
            return ssi->last_miso;
        break;
    case BITBANG_SSI_MOSI:
        ssi->last_mosi = level;
        return ssi->last_miso;
    case BITBANG_SSI_MISO:
        return ssi->last_miso;
    }

    DPRINTF("sclk %d->%d (state: %s)\n", ssi->last_sclk, level, ssi->state == ACTIVE ? "active" : "idle");

    bitbang_ssi_sclk_state sclk_state = -1;
    if (ssi->last_sclk == 0 && level == 1) {
        switch(ssi->cpol_mode) {
        case BITBANG_SSI_CPOL0:
            sclk_state = IDLE2ACTIVE;
            break;
        case BITBANG_SSI_CPOL1:
            sclk_state = ACTIVE2IDLE;
            break;
        }
    } else if (ssi->last_sclk == 1 && level == 0) {
        switch(ssi->cpol_mode) {
        case BITBANG_SSI_CPOL0:
            sclk_state = ACTIVE2IDLE;
            break;
        case BITBANG_SSI_CPOL1:
            sclk_state = IDLE2ACTIVE;
            break;
        }
    } else {
        abort();
    }
    ssi->last_sclk = level;

    if (ssi->state == IDLE) {
        ssi->state = ACTIVE;
        ssi->data_mosi = 0;
        ssi->data_mosi_count = 0;

        if (ssi->cpha_mode == BITBANG_SSI_CPHA1) {
            return ssi->last_miso;
        }
    }

    if (ssi->cpha_mode == BITBANG_SSI_CPHA0) {
        switch(sclk_state) {
        case IDLE2ACTIVE:
            bitbang_ssi_set_mosi(ssi);
            break;
        case ACTIVE2IDLE:
            bitbang_ssi_get_miso(ssi);
            break;
        }
    } else if (ssi->cpha_mode == BITBANG_SSI_CPHA1) {
        switch(sclk_state) {
        case IDLE2ACTIVE:
            bitbang_ssi_get_miso(ssi);
            break;
        case ACTIVE2IDLE:
            bitbang_ssi_set_mosi(ssi);
            break;
        }
    }

    if (ssi->data_mosi_count == ssi->transfer_size) {
        ssi->data_miso = reverse_bits(ssi_transfer(ssi->bus, ssi->data_mosi));
        ssi->data_miso_count = ssi->transfer_size;
        printf("bitbang_ssi: transfer(%x) -> %x\n", ssi->data_mosi, ssi->data_miso);
        ssi->state = IDLE;
    }
    return ssi->last_miso;
}

bitbang_ssi_interface *bitbang_ssi_init(SSIBus *bus, bitbang_ssi_cpol_mode cpol_mode, bitbang_ssi_cpha_mode cpha_mode, int transfer_size)
{
    DPRINTF("init(cpol:%d, cpha:%d, size:%d)\n", cpol_mode, cpha_mode, transfer_size);
    bitbang_ssi_interface *ssi;

    ssi = g_malloc0(sizeof(bitbang_ssi_interface));

    ssi->bus = bus;

    // Configuration
    assert(transfer_size < (sizeof(ssi->data_miso)*8));
    ssi->transfer_size = transfer_size;
    ssi->cpol_mode = cpol_mode;
    ssi->cpha_mode = cpha_mode;

    // Pin values
    switch (cpol_mode) {
    case BITBANG_SSI_CPOL0:
        ssi->last_sclk = 0;
        break;
    case BITBANG_SSI_CPOL1:
        ssi->last_sclk = 1;
        break;
    }
    ssi->last_mosi = 0;
    ssi->last_miso = 0;

    // Data transmission state
    ssi->state = IDLE;
    ssi->data_miso = 0;
    ssi->data_miso_count = 0;
    ssi->data_mosi = 0;
    ssi->data_mosi_count = 0;

    return ssi;
}

/* GPIO interface.  */

#define TYPE_GPIO_SSI "gpio_ssi"
#define GPIO_SSI(obj) OBJECT_CHECK(GPIOSSIState, (obj), TYPE_GPIO_SSI)

typedef struct GPIOSSIState {
    SysBusDevice parent_obj;

    MemoryRegion dummy_iomem;
    bitbang_ssi_interface *bitbang;
    int last_level;
    qemu_irq out;
} GPIOSSIState;

static void bitbang_ssi_gpio_set(void *opaque, int irq, int level)
{
    GPIOSSIState *s = opaque;

    level = bitbang_ssi_set(s->bitbang, irq, level);
    if (level != s->last_level) {
        s->last_level = level;
        qemu_set_irq(s->out, level);
    }
}

static void gpio_ssi_init(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    GPIOSSIState *s = GPIO_SSI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    SSIBus *bus;

    memory_region_init(&s->dummy_iomem, obj, "gpio_ssi", 0);
    sysbus_init_mmio(sbd, &s->dummy_iomem);

    bus = ssi_create_bus(dev, "ssi");
    s->bitbang = bitbang_ssi_init(bus, BITBANG_SSI_CPOL0, BITBANG_SSI_CPHA0, 8);

    qdev_init_gpio_in(dev, bitbang_ssi_gpio_set, 2);
    qdev_init_gpio_out(dev, &s->out, 1);
}

static void gpio_ssi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    dc->desc = "Virtual GPIO to SSI bridge";
}

static const TypeInfo gpio_ssi_info = {
    .name          = TYPE_GPIO_SSI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GPIOSSIState),
    .instance_init = gpio_ssi_init,
    .class_init    = gpio_ssi_class_init,
};

static void bitbang_ssi_register_types(void)
{
    type_register_static(&gpio_ssi_info);
}

type_init(bitbang_ssi_register_types)
