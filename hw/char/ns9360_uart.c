/*
 * Digi NS9360 UART device model
 *
 * Models the NS9360 serial port (UART Port A) with the register layout
 * as documented in the NS9360 hardware reference manual.
 *
 * Register map (offsets from base):
 *   0x00  CTRL_A       - Control register A (r/w)
 *   0x04  CTRL_B       - Control register B (r/w)
 *   0x08  STAT_A       - Status register A (read-only)
 *   0x0C  BITRATE      - Baud rate register (r/w)
 *   0x10  FIFO         - TX/RX FIFO data register
 *   0x18  RX_CHAR_TMR  - RX character timer (r/w)
 *
 * Copyright (c) 2025 The QEMU contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "chardev/char-fe.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

/* Register offsets from device base */
#define NS9360_UART_CTRL_A      0x00
#define NS9360_UART_CTRL_B      0x04
#define NS9360_UART_STAT_A      0x08
#define NS9360_UART_BITRATE     0x0C
#define NS9360_UART_FIFO        0x10
#define NS9360_UART_RX_CHAR_TMR 0x18

/* STAT_A register bits */
#define NS9360_UART_STAT_TRDY   (1 << 3)   /* TX ready */
#define NS9360_UART_STAT_RRDY   (1 << 11)  /* RX ready (data available) */

/* Device MMIO region size (covers offsets 0x00 - 0x1B) */
#define NS9360_UART_MMIO_SIZE   0x20

#define TYPE_NS9360_UART "ns9360-uart"
OBJECT_DECLARE_SIMPLE_TYPE(NS9360UartState, NS9360_UART)

struct NS9360UartState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    CharBackend chr;

    uint32_t ctrl_a;
    uint32_t ctrl_b;
    uint32_t stat_a;
    uint32_t bitrate;
    uint32_t rx_char_tmr;
    uint8_t  rx_fifo;       /* single-character RX holding register */
};

static int ns9360_uart_can_rx(void *opaque)
{
    NS9360UartState *s = opaque;

    /* Accept a character only if there is no unread character pending */
    return !(s->stat_a & NS9360_UART_STAT_RRDY);
}

static void ns9360_uart_rx(void *opaque, const uint8_t *buf, int size)
{
    NS9360UartState *s = opaque;

    s->rx_fifo = buf[0];
    s->stat_a |= NS9360_UART_STAT_RRDY;
}

static void ns9360_uart_event(void *opaque, QEMUChrEvent event)
{
    /* Nothing to do */
}

static uint64_t ns9360_uart_read(void *opaque, hwaddr offset, unsigned size)
{
    NS9360UartState *s = opaque;

    switch (offset) {
    case NS9360_UART_CTRL_A:
        return s->ctrl_a;

    case NS9360_UART_CTRL_B:
        return s->ctrl_b;

    case NS9360_UART_STAT_A:
        return s->stat_a;

    case NS9360_UART_BITRATE:
        return s->bitrate;

    case NS9360_UART_FIFO:
        /*
         * Reading the FIFO register returns the received character
         * and clears the RRDY flag.  We must notify the chardev
         * backend that we can accept more input, otherwise it will
         * stop polling the underlying transport after can_rx returns 0.
         */
        {
            uint8_t ch = s->rx_fifo;
            s->stat_a &= ~NS9360_UART_STAT_RRDY;
            qemu_chr_fe_accept_input(&s->chr);
            return ch;
        }

    case NS9360_UART_RX_CHAR_TMR:
        return s->rx_char_tmr;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "ns9360-uart: read from unknown register 0x"
                      HWADDR_FMT_plx "\n", offset);
        return 0;
    }
}

static void ns9360_uart_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    NS9360UartState *s = opaque;
    uint8_t ch;

    switch (offset) {
    case NS9360_UART_CTRL_A:
        s->ctrl_a = value;
        break;

    case NS9360_UART_CTRL_B:
        s->ctrl_b = value;
        break;

    case NS9360_UART_STAT_A:
        /* Status register is read-only; ignore writes */
        break;

    case NS9360_UART_BITRATE:
        s->bitrate = value;
        break;

    case NS9360_UART_FIFO:
        /*
         * Writing the FIFO register transmits a character via the
         * serial backend.
         */
        ch = (uint8_t)value;
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
        break;

    case NS9360_UART_RX_CHAR_TMR:
        s->rx_char_tmr = value;
        break;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "ns9360-uart: write to unknown register 0x"
                      HWADDR_FMT_plx " = 0x%" PRIx64 "\n",
                      offset, value);
        break;
    }
}

static const MemoryRegionOps ns9360_uart_ops = {
    .read = ns9360_uart_read,
    .write = ns9360_uart_write,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void ns9360_uart_reset(DeviceState *dev)
{
    NS9360UartState *s = NS9360_UART(dev);

    s->ctrl_a = 0;
    s->ctrl_b = 0;
    s->stat_a = NS9360_UART_STAT_TRDY;  /* TX always ready after reset */
    s->bitrate = 0;
    s->rx_char_tmr = 0;
    s->rx_fifo = 0;
}

static void ns9360_uart_realize(DeviceState *dev, Error **errp)
{
    NS9360UartState *s = NS9360_UART(dev);

    qemu_chr_fe_set_handlers(&s->chr,
                             ns9360_uart_can_rx,
                             ns9360_uart_rx,
                             ns9360_uart_event,
                             NULL, s, NULL, true);
}

static void ns9360_uart_init(Object *obj)
{
    NS9360UartState *s = NS9360_UART(obj);

    memory_region_init_io(&s->iomem, obj, &ns9360_uart_ops, s,
                          TYPE_NS9360_UART, NS9360_UART_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_ns9360_uart = {
    .name = "ns9360-uart",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl_a, NS9360UartState),
        VMSTATE_UINT32(ctrl_b, NS9360UartState),
        VMSTATE_UINT32(stat_a, NS9360UartState),
        VMSTATE_UINT32(bitrate, NS9360UartState),
        VMSTATE_UINT32(rx_char_tmr, NS9360UartState),
        VMSTATE_UINT8(rx_fifo, NS9360UartState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property ns9360_uart_properties[] = {
    DEFINE_PROP_CHR("chardev", NS9360UartState, chr),
};

static void ns9360_uart_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ns9360_uart_realize;
    device_class_set_legacy_reset(dc, ns9360_uart_reset);
    dc->vmsd = &vmstate_ns9360_uart;
    device_class_set_props(dc, ns9360_uart_properties);
}

static const TypeInfo ns9360_uart_info = {
    .name          = TYPE_NS9360_UART,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NS9360UartState),
    .instance_init = ns9360_uart_init,
    .class_init    = ns9360_uart_class_init,
};

static void ns9360_uart_register_types(void)
{
    type_register_static(&ns9360_uart_info);
}

type_init(ns9360_uart_register_types)
