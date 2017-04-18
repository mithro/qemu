/*
 *  QEMU model of the LiteEth block.
 *
 *  Copyright (c) 2016 Ramtin Amin <keytwo@gmail.com>
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
 *
 *
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "cpu.h"
#include "hw/hw.h"
#include "hw/mdio/mdio.h"
#include "hw/sysbus.h"
#include "trace.h"
#include "net/net.h"
#include "qemu/error-report.h"

#define LITEETH_BUFFER_SIZE 0x800

enum {
    R_ETHMAC_SRAM_WRITER_SLOT,
    R_ETHMAC_SRAM_WRITER_LENGTH0,
    R_ETHMAC_SRAM_WRITER_LENGTH1,
    R_ETHMAC_SRAM_WRITER_LENGTH2,
    R_ETHMAC_SRAM_WRITER_LENGTH3,
    R_ETHMAC_SRAM_WRITER_EV_STATUS,
    R_ETHMAC_SRAM_WRITER_EV_PENDING,
    R_ETHMAC_SRAM_WRITER_EV_ENABLE,
    R_ETHMAC_SRAM_READER_START,
    R_ETHMAC_SRAM_READER_READY,
    R_ETHMAC_SRAM_READER_SLOT,
    R_ETHMAC_SRAM_READER_LENGTH0,
    R_ETHMAC_SRAM_READER_LENGTH1,
    R_ETHMAC_SRAM_READER_EV_STATUS,
    R_ETHMAC_SRAM_READER_EV_PENDING,
    R_ETHMAC_SRAM_READER_EV_ENABLE,
    R_ETHMAC_PREAMBLE_CRC,
    R_MAX,
};

enum {
    R_ETHPHY_CRG_RESET,
    R_ETHPHY_MDIO_W,
    R_ETHPHY_MDIO_R,
    R_PHY_MAX
};

/* MDIO interface */
enum {
    SETUP_PHY_RST = (1<<0),
};

enum {
    // ETHPHY_MDIO_W
    MDIO_CLK = 0x01,
    MDIO_OE  = 0x02,
    MDIO_DO  = 0x04,
    // ETHPHY_MDIO_W
    MDIO_DI  = 0x01,
};

enum {
    STATE_EMPTY   = 0,
    STATE_LOADED  = 1,
    STATE_PENDING = 2,
};

enum {
    MDIO_OP_WRITE = 1,
    MDIO_OP_READ  = 2,
};

enum mdio_bb_state {
    MDIO_STATE_IDLE,
    MDIO_STATE_READING,
    MDIO_STATE_WRITING,
};

enum {
    R_MDIO_ID1  = 2,
    R_MDIO_ID2  = 3,
    R_MDIO_MAX  = 32
};

struct LiteEthMdioBBState {
    int last_clk;
    int count;
    uint32_t data;
    uint16_t data_out;
    int state;

    uint8_t phy_addr;
    uint8_t reg_addr;

    uint16_t regs[R_MDIO_MAX];
};
typedef struct LiteEthMdioBBState LiteEthMdioBBState;


struct LiteEthState {
    SysBusDevice parent_obj;

    NICState *nic;
    NICConf conf;
    char *phy_model;
    MemoryRegion buffers;
    MemoryRegion regs_region;
    MemoryRegion phy_regs_region;
    qemu_irq irq;
    uint8_t irq_state;
    uint32_t regs[R_MAX];
    uint16_t phy_regs[R_PHY_MAX];

    LiteEthMdioBBState mdio_bb;
    struct MDIOBus *mdio_bus;

    uint8_t *tx0_buf;
    uint8_t *tx1_buf;
    uint8_t *rx0_buf;
    uint8_t *rx1_buf;
    uint8_t *rx_buf;
    uint8_t *tx_buf;
};

typedef struct LiteEthState LiteEthState;

#define TYPE_LITEETH "litex-liteeth"
#define LITEETH(obj) \
    OBJECT_CHECK(LiteEthState, (obj), TYPE_LITEETH)


static void liteeth_update_mdio_bb(LiteEthState *s, uint16_t phy_mdio_write_reg_value, uint16_t *phy_mdio_read_reg)
{
    LiteEthMdioBBState* m = &(s->mdio_bb);

    /* detect rising clk edge */
    if (m->last_clk == 0 && (phy_mdio_write_reg_value & MDIO_CLK)) {
        /* shift data in */
        int bit = ((phy_mdio_write_reg_value & MDIO_DO)
                   && (phy_mdio_write_reg_value & MDIO_OE)) ? 1 : 0;
        m->data = (m->data << 1) | bit;

        /* check for sync */
        if (m->data == 0xffffffff) {
            printf("Datasync\n");
            m->count = 32;
        }

        if (m->count == 16) {
            /*
            struct {
                uint16_t start   : 3; // 3
                uint16_t op      : 1; // 4
                uint16_t phyaddr : 5; // 9
                uint16_t reg     : 5; // 15
                uint16_t ta      : 1; // 16
            };
            */
            uint8_t start = (m->data >> 14) & 0x3; // start
            uint8_t op    = (m->data >> 12) & 0x3; // read/write
            uint8_t ta    = (m->data      ) & 0x3;

            if (start == 1 && op == MDIO_OP_WRITE && ta == 2) {
                m->state = MDIO_STATE_WRITING;
            } else if (start == 1 && op == MDIO_OP_READ && (ta & 1) == 0) {
                m->state = MDIO_STATE_READING;
            } else {
                m->state = MDIO_STATE_IDLE;
            }

            if (m->state != MDIO_STATE_IDLE) {
                m->phy_addr = (m->data >> 7) & 0x1f;
                m->reg_addr = (m->data >> 2) & 0x1f;
            }

            if (m->state == MDIO_STATE_READING) {
                if (s->mdio_bus) {
                    m->data_out = mdio_recv(s->mdio_bus, m->phy_addr, m->reg_addr);
                } else {
                    m->data_out = 0;
                }
                printf("Reading value %08x fr MDIO PHY %08x REG %08x\n", (unsigned int)(m->data_out), (unsigned int)(m->phy_addr), (unsigned int)(m->reg_addr));
            }
        }

        if (m->count < 16 && m->state == MDIO_STATE_READING) {
            int bit = (m->data_out & 0x8000) ? 1 : 0;
            m->data_out <<= 1;

            if (bit) {
                (*phy_mdio_read_reg) |= MDIO_DI;
            } else {
                (*phy_mdio_read_reg) &= ~MDIO_DI;
            }
        }

        if (m->count == 0) {
            if (m->state == MDIO_STATE_WRITING) {
                uint16_t data = m->data & 0xffff;

                printf("Writing value %08x to MDIO PHY %08x REG %08x\n", (unsigned int)(data), (unsigned int)(m->phy_addr), (unsigned int)(m->reg_addr));
                if (s->mdio_bus) {
                    mdio_send(s->mdio_bus, m->phy_addr, m->reg_addr, data);
                }
            }
            m->state = MDIO_STATE_IDLE;
        }
        m->count--;
    }

    m->last_clk = (phy_mdio_write_reg_value & MDIO_CLK) ? 1 : 0;
}

static uint64_t liteeth_phy_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    LiteEthState *s = opaque;
    addr >>= 2;

    uint64_t r = s->phy_regs[addr];
    return r;
}

static void liteeth_phy_reg_write(void *opaque, hwaddr addr, uint64_t value,  unsigned size)
{
    LiteEthState *s = opaque;
    addr >>= 2;

    s->phy_regs[addr] = value & 0xff;
    switch(addr)
    {
    case R_ETHPHY_MDIO_W:
        liteeth_update_mdio_bb(s, s->phy_regs[R_ETHPHY_MDIO_W], &(s->phy_regs[R_ETHPHY_MDIO_R]));
    }
}

static ssize_t liteeth_rx(NetClientState *nc, const uint8_t *buf, size_t size)
{
    LiteEthState *s = qemu_get_nic_opaque(nc);
    //int i;
    size_t tmpsize;

    //printf("\n[QEMU] ethernet rx %lu\n", size);
    /*for (i = 0; i < size; i++)
    {
        printf("%02x ", buf[i]);
    }
    printf("\n");
    */
    if(s->regs[R_ETHMAC_SRAM_WRITER_EV_PENDING])
        return 0;

    if(s->rx_buf == s->rx0_buf)
    {
        s->rx_buf = s->rx1_buf;
        s->regs[R_ETHMAC_SRAM_WRITER_SLOT] = 1;
    } else {
        s->rx_buf = s->rx0_buf;
        s->regs[R_ETHMAC_SRAM_WRITER_SLOT] = 0;
    }

    memset(s->rx_buf, 0, LITEETH_BUFFER_SIZE);

    if(size < LITEETH_BUFFER_SIZE)
    {
        memcpy(s->rx_buf, buf, size);
    }
    else
    {
        memcpy(s->rx_buf, buf, LITEETH_BUFFER_SIZE);
    }


    tmpsize = (size < 60)?60:size;
    s->regs[R_ETHMAC_SRAM_WRITER_LENGTH0] = (tmpsize << 24 ) & 0xff;
    s->regs[R_ETHMAC_SRAM_WRITER_LENGTH1] = (tmpsize << 16 ) & 0xff;
    s->regs[R_ETHMAC_SRAM_WRITER_LENGTH2] = (tmpsize << 8 ) & 0xff;
    s->regs[R_ETHMAC_SRAM_WRITER_LENGTH3] = tmpsize & 0xff;
    s->regs[R_ETHMAC_SRAM_WRITER_EV_PENDING] =  1;

    if(s->regs[R_ETHMAC_SRAM_WRITER_EV_ENABLE])
        {
            if(!s->irq_state)
            {
                qemu_irq_raise(s->irq);
                s->irq_state = 1;
            }

        }


    return size;
}

static uint64_t liteeth_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    LiteEthState *s = opaque;
    uint32_t r = 0;
    addr >>= 2;
    r = s->regs[addr];
    if (addr == 9)
        r = 1;

    //printf("Reading addr %08x value %08x\n", (unsigned int)addr, (unsigned int)r);
    return r;
}



static void liteeth_reg_write(void *opaque, hwaddr addr, uint64_t value,  unsigned size)
{
    LiteEthState *s = opaque;
    //int i;
    uint32_t len;
    addr >>= 2;

    //printf("[QEMU] Writing value %08x on addr %08x\n", (unsigned int)value, (unsigned int)addr);
    s->regs[addr] = value & 0xff;
    switch(addr)
    {
    case R_ETHMAC_SRAM_WRITER_SLOT:
        printf("rx slot change %d\n",(unsigned int)value);
        if(value)
        {
            s->rx_buf = s->rx1_buf;
        }
        else
        {
            s->rx_buf = s->rx0_buf;
        }
        break;
    case R_ETHMAC_SRAM_READER_SLOT:
      //printf("[QEMU] tx slot change %d\n",(unsigned int)value);
        if(value)
        {
            s->tx_buf = s->tx1_buf;
        }
        else
        {
            s->tx_buf = s->tx0_buf;
        }
        break;
    case R_ETHMAC_SRAM_READER_START:

        len = (s->regs[R_ETHMAC_SRAM_READER_LENGTH0] << 8) |    \
          (s->regs[R_ETHMAC_SRAM_READER_LENGTH1]  & 0xff);
        //printf("[QEMU] len = %d\n", len);
        qemu_send_packet_raw(qemu_get_queue(s->nic), s->tx_buf, len);

        s->regs[R_ETHMAC_SRAM_READER_EV_PENDING] = 1;

        if(s->regs[R_ETHMAC_SRAM_READER_EV_ENABLE])
        {
          if(!s->irq_state)
          {
            qemu_irq_raise(s->irq);
            s->irq_state = 1;
          }
        }
        break;

    case R_ETHMAC_SRAM_READER_EV_PENDING:
    case R_ETHMAC_SRAM_WRITER_EV_PENDING:
        s->regs[addr] = 0;
        if(addr == R_ETHMAC_SRAM_WRITER_EV_PENDING)
        {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }

        if(s->regs[R_ETHMAC_SRAM_WRITER_EV_ENABLE])
        {
          if(s->regs[R_ETHMAC_SRAM_WRITER_EV_PENDING])
            break;
        }

        if(s->regs[R_ETHMAC_SRAM_READER_EV_ENABLE])
        {
          if(s->regs[R_ETHMAC_SRAM_READER_EV_PENDING])
            break;
        }
        if(s->irq_state)
        {
          qemu_irq_lower(s->irq);
          s->irq_state = 0;
        }
        break;

    case R_ETHMAC_SRAM_READER_EV_ENABLE:
      if(s->regs[R_ETHMAC_SRAM_READER_EV_PENDING])
        if(!s->irq_state)
        {
          qemu_irq_raise(s->irq);
          s->irq_state = 1;
        }
      break;

    case R_ETHMAC_SRAM_WRITER_EV_ENABLE:
      if(s->regs[R_ETHMAC_SRAM_WRITER_EV_PENDING])
        if(!s->irq_state)
        {
          qemu_irq_raise(s->irq);
          s->irq_state = 1;
        }
      break;
    default:

        break;
    }

};


static void liteeth_reset(DeviceState *d)
{
    LiteEthState *s = LITEETH(d);
    int i;

    s->irq_state = 0;

    for (i = 0; i < R_MAX; i++) {
        s->regs[i] = 0;
    }
    for (i = 0; i < R_PHY_MAX; i++) {
        s->phy_regs[i] = 0;
    }
}

static NetClientInfo net_liteeth_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = liteeth_rx,
};


static const MemoryRegionOps liteeth_reg_ops = {
    .read = liteeth_reg_read,
    .write = liteeth_reg_write,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps liteeth_phy_ops = {
    .read = liteeth_phy_reg_read,
    .write = liteeth_phy_reg_write,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .endianness = DEVICE_NATIVE_ENDIAN,
};


static int liteeth_init(SysBusDevice *sbd)
{
    DeviceState *dev = DEVICE(sbd);
    LiteEthState *s = LITEETH(dev);
    size_t buffers_size = TARGET_PAGE_ALIGN(3 * LITEETH_BUFFER_SIZE);

    sysbus_init_irq(sbd, &s->irq);

    memory_region_init_io(&s->phy_regs_region, OBJECT(dev), &liteeth_phy_ops, s, "liteeth_phy_regs", R_PHY_MAX * 4);
    sysbus_init_mmio(sbd, &s->phy_regs_region);

    memory_region_init_io(&s->regs_region, OBJECT(dev), &liteeth_reg_ops, s, "liteeth_regs", R_MAX * 4);
    sysbus_init_mmio(sbd, &s->regs_region);

    s->mdio_bus = mdio_init_bus(dev, "mdio-bus");
    mdio_create_slave(s->mdio_bus, "88e1116r", 0x0);

    /* register buffers memory */
    memory_region_init_ram(&s->buffers, OBJECT(dev), "liteeth.buffers", buffers_size, &error_fatal);
    vmstate_register_ram_global(&s->buffers);
    s->rx0_buf = memory_region_get_ram_ptr(&s->buffers);
    s->rx1_buf = s->rx0_buf + LITEETH_BUFFER_SIZE;
    s->tx0_buf = s->rx1_buf + LITEETH_BUFFER_SIZE;
    s->tx1_buf = s->tx0_buf + LITEETH_BUFFER_SIZE;
    s->tx_buf = s->tx1_buf;
    s->rx_buf = s->rx1_buf;
    s->irq_state = 0;

    memset(s->rx0_buf, 'a', LITEETH_BUFFER_SIZE);
    memset(s->rx1_buf, 'b', LITEETH_BUFFER_SIZE);
    memset(s->tx0_buf, 'c', LITEETH_BUFFER_SIZE);
    memset(s->tx1_buf, 'd', LITEETH_BUFFER_SIZE);

    sysbus_init_mmio(sbd, &s->buffers);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_liteeth_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);

    return 0;
}


static const VMStateDescription vmstate_liteeth_mdio_bb = {
    .name = "liteeth-mdio-bb",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_INT32(last_clk, LiteEthMdioBBState),
        VMSTATE_INT32(count, LiteEthMdioBBState),
        VMSTATE_UINT32(data, LiteEthMdioBBState),
        VMSTATE_UINT16(data_out, LiteEthMdioBBState),
        VMSTATE_INT32(state, LiteEthMdioBBState),
        VMSTATE_UINT8(phy_addr, LiteEthMdioBBState),
        VMSTATE_UINT8(reg_addr, LiteEthMdioBBState),
        VMSTATE_UINT16_ARRAY(regs, LiteEthMdioBBState, R_MDIO_MAX),
        VMSTATE_END_OF_LIST()
    }
};


static const VMStateDescription vmstate_liteeth = {
    .name = "liteeth",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, LiteEthState, R_MAX),
        VMSTATE_UINT16_ARRAY(phy_regs, LiteEthState, R_PHY_MAX),
        VMSTATE_STRUCT(mdio_bb, LiteEthState, 0,
                vmstate_liteeth_mdio_bb, LiteEthMdioBBState),
        VMSTATE_END_OF_LIST()
    }
};

static Property liteeth_properties[] = {
    DEFINE_NIC_PROPERTIES(LiteEthState, conf),
    DEFINE_PROP_STRING("phy_model", LiteEthState, phy_model),
    DEFINE_PROP_END_OF_LIST(),
};

static void liteeth_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SysBusDeviceClass *k = SYS_BUS_DEVICE_CLASS(klass);

    k->init = liteeth_init;
    dc->reset = liteeth_reset;
    dc->vmsd = &vmstate_liteeth;
    dc->props = liteeth_properties;
}

static const TypeInfo liteeth_info = {
    .name          = TYPE_LITEETH,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(LiteEthState),
    .class_init    = liteeth_class_init,
};

static void liteeth_register_types(void)
{
    type_register_static(&liteeth_info);
}

type_init(liteeth_register_types)
