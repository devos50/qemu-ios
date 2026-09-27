/*
 * Samsung DSIM MIPI DSI master of the S5L8720, with the Pinot LCD panel attached.
 *
 * iBoot brings the link up (reset, timings, stop state, HS clock) and reads the
 * panel ID with a generic read; the kernel only restores the registers it saved
 * at start-up when it powers the display back on, and toggles the HS clock.
 * Packets complete instantly, so the header and payload FIFOs are always empty.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/arm/ipod_touch_mipi_dsi.h"
#include "trace.h"

#define REG(s, addr) ((s)->regs[(addr) / 4])

static void mipi_dsi_update_irq(IPodTouchMIPIDSIState *s)
{
    qemu_set_irq(s->irq, !!(REG(s, REG_INTSRC) & ~REG(s, REG_INTMSK)));
}

static void mipi_dsi_flush_fifos(IPodTouchMIPIDSIState *s)
{
    s->payload_words = 0;
    s->rx_head = 0;
    s->rx_count = 0;
}

static void mipi_dsi_rx_push(IPodTouchMIPIDSIState *s, uint32_t word)
{
    if (s->rx_count == MIPI_DSI_RX_FIFO_WORDS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: RX FIFO overflow\n", __func__);
        return;
    }
    s->rx_fifo[(s->rx_head + s->rx_count) % MIPI_DSI_RX_FIFO_WORDS] = word;
    s->rx_count++;
}

static uint32_t mipi_dsi_rx_pop(IPodTouchMIPIDSIState *s)
{
    uint32_t word;

    if (s->rx_count == 0) {
        // The kernel reads every register, RXFIFO included, when it starts
        return 0;
    }
    word = s->rx_fifo[s->rx_head];
    s->rx_head = (s->rx_head + 1) % MIPI_DSI_RX_FIFO_WORDS;
    s->rx_count--;
    return word;
}

static bool mipi_dsi_is_long_packet(uint8_t type)
{
    switch (type) {
    case 0x09: case 0x19: case 0x29: case 0x39:
    case 0x0c: case 0x1c: case 0x2c: case 0x3c:
    case 0x0d: case 0x1d: case 0x2d: case 0x3d:
    case 0x0e: case 0x1e: case 0x2e: case 0x3e:
        return true;
    default:
        return false;
    }
}

static void mipi_dsi_panel_read(IPodTouchMIPIDSIState *s, uint8_t type, uint8_t reg)
{
    if (type != DSI_GENERIC_READ_1 || reg != PINOT_REG_PANEL_ID) {
        qemu_log_mask(LOG_UNIMP, "%s: read of type 0x%02x from panel register 0x%02x\n",
                      __func__, type, reg);
        return;
    }

    // Long read response with the ID bytes in order, most significant first
    mipi_dsi_rx_push(s, DSI_RSP_GENERIC_LONG_READ | (3 << 8));
    mipi_dsi_rx_push(s, ((s->panel_id >> 16) & 0xff) |
                        (s->panel_id & 0xff00) |
                        ((s->panel_id & 0xff) << 16));
    REG(s, REG_INTSRC) |= rDSIM_INTSRC_RxDatDone;
}

static void mipi_dsi_send_packet(IPodTouchMIPIDSIState *s, uint32_t header)
{
    uint8_t type = header & 0x3f;
    uint8_t data0 = (header >> 8) & 0xff;
    uint8_t data1 = (header >> 16) & 0xff;

    trace_ipod_touch_mipi_dsi_packet(type, data0, data1);

    if (mipi_dsi_is_long_packet(type)) {
        uint32_t words = DIV_ROUND_UP((header >> 8) & 0xffff, 4);
        s->payload_words -= MIN(words, s->payload_words);
        return;
    }

    switch (type) {
    case DSI_GENERIC_READ_0:
    case DSI_GENERIC_READ_1:
    case DSI_GENERIC_READ_2:
    case DSI_DCS_READ:
        mipi_dsi_panel_read(s, type, data0);
        break;
    default:
        break;
    }
}

static uint32_t mipi_dsi_status(IPodTouchMIPIDSIState *s)
{
    uint32_t lanes = (REG(s, REG_CONFIG) >> rDSIM_CONFIG_LaneEnShift) & 0xf;
    uint32_t status = lanes;

    // The clock lane leaves the stop state while it runs in high speed
    if (REG(s, REG_CLKCTRL) & rDSIM_CLKCTRL_TxRequestHsClk) {
        status |= rDSIM_STATUS_TxReadyHsClk;
    } else {
        status |= rDSIM_STATUS_StopstateClk;
    }
    if (REG(s, REG_PLLCTRL) & rDSIM_PLLCTRL_PllEn) {
        status |= rDSIM_STATUS_PllStable;
    }
    return status;
}

static uint64_t ipod_touch_mipi_dsi_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchMIPIDSIState *s = IPOD_TOUCH_MIPI_DSI(opaque);
    uint32_t val;

    switch (addr) {
    case REG_STATUS:
        val = mipi_dsi_status(s);
        break;
    case REG_RXFIFO:
        val = mipi_dsi_rx_pop(s);
        break;
    case REG_FIFOCTRL:
        val = (REG(s, REG_FIFOCTRL) & rDSIM_FIFOCTRL_nInitMask) | rDSIM_FIFOCTRL_EmptyHSfr;
        if (s->payload_words == 0) {
            val |= rDSIM_FIFOCTRL_EmptyPayload;
        }
        if (s->rx_count == 0) {
            val |= rDSIM_FIFOCTRL_EmptyRx;
        }
        break;
    case REG_SWRST:
    case REG_PKTHDR:
    case REG_PAYLOAD:
        val = 0;
        break;
    default:
        if (addr >= MIPI_DSI_NUM_REGS * 4) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: read from unknown register 0x%03" HWADDR_PRIx "\n",
                          __func__, addr);
            return 0;
        }
        val = REG(s, addr);
        break;
    }

    trace_ipod_touch_mipi_dsi_read(addr, val);
    return val;
}

static void ipod_touch_mipi_dsi_reset(DeviceState *dev);

static void ipod_touch_mipi_dsi_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchMIPIDSIState *s = IPOD_TOUCH_MIPI_DSI(opaque);

    trace_ipod_touch_mipi_dsi_write(addr, val);

    switch (addr) {
    case REG_STATUS:
    case REG_RXFIFO:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write of 0x%08" PRIx64 " to read-only register 0x%03" HWADDR_PRIx "\n",
                      __func__, val, addr);
        return;
    case REG_SWRST:
        if (val & rDSIM_SWRST_SwRst) {
            ipod_touch_mipi_dsi_reset(DEVICE(s));
            REG(s, REG_INTSRC) = rDSIM_INTSRC_SwRstRelease;
        }
        break;
    case REG_INTSRC:
        REG(s, REG_INTSRC) &= ~val;
        break;
    case REG_PKTHDR:
        mipi_dsi_send_packet(s, val);
        break;
    case REG_PAYLOAD:
        s->payload_words++;
        break;
    case REG_FIFOCTRL:
        // Clearing any of the nInit bits resets the FIFO pointers
        if ((val & rDSIM_FIFOCTRL_nInitMask) != rDSIM_FIFOCTRL_nInitMask) {
            mipi_dsi_flush_fifos(s);
        }
        REG(s, REG_FIFOCTRL) = val & rDSIM_FIFOCTRL_nInitMask;
        break;
    case REG_PLLCTRL:
        if ((val & rDSIM_PLLCTRL_PllEn) && !(REG(s, REG_PLLCTRL) & rDSIM_PLLCTRL_PllEn)) {
            REG(s, REG_INTSRC) |= rDSIM_INTSRC_PllStable;
        }
        REG(s, REG_PLLCTRL) = val;
        break;
    default:
        if (addr >= MIPI_DSI_NUM_REGS * 4) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: write of 0x%08" PRIx64 " to unknown register 0x%03" HWADDR_PRIx "\n",
                          __func__, val, addr);
            return;
        }
        REG(s, addr) = val;
        break;
    }

    mipi_dsi_update_irq(s);
}

static const MemoryRegionOps mipi_dsi_ops = {
    .read = ipod_touch_mipi_dsi_read,
    .write = ipod_touch_mipi_dsi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void ipod_touch_mipi_dsi_reset(DeviceState *dev)
{
    IPodTouchMIPIDSIState *s = IPOD_TOUCH_MIPI_DSI(dev);

    memset(s->regs, 0, sizeof(s->regs));
    REG(s, REG_FIFOCTRL) = rDSIM_FIFOCTRL_nInitMask;
    mipi_dsi_flush_fifos(s);
    mipi_dsi_update_irq(s);
}

static void ipod_touch_mipi_dsi_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    IPodTouchMIPIDSIState *s = IPOD_TOUCH_MIPI_DSI(obj);

    memory_region_init_io(&s->iomem, obj, &mipi_dsi_ops, s, "mipi_dsi", 0x10000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static Property ipod_touch_mipi_dsi_properties[] = {
    DEFINE_PROP_UINT32("panel-id", IPodTouchMIPIDSIState, panel_id, PINOT_DEFAULT_PANEL_ID),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_ipod_touch_mipi_dsi = {
    .name = TYPE_IPOD_TOUCH_MIPI_DSI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IPodTouchMIPIDSIState, MIPI_DSI_NUM_REGS),
        VMSTATE_UINT32(payload_words, IPodTouchMIPIDSIState),
        VMSTATE_UINT32_ARRAY(rx_fifo, IPodTouchMIPIDSIState, MIPI_DSI_RX_FIFO_WORDS),
        VMSTATE_UINT32(rx_head, IPodTouchMIPIDSIState),
        VMSTATE_UINT32(rx_count, IPodTouchMIPIDSIState),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_mipi_dsi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = ipod_touch_mipi_dsi_reset;
    dc->vmsd = &vmstate_ipod_touch_mipi_dsi;
    device_class_set_props(dc, ipod_touch_mipi_dsi_properties);
}

static const TypeInfo ipod_touch_mipi_dsi_info = {
    .name          = TYPE_IPOD_TOUCH_MIPI_DSI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchMIPIDSIState),
    .instance_init = ipod_touch_mipi_dsi_init,
    .class_init    = ipod_touch_mipi_dsi_class_init,
};

static void ipod_touch_machine_types(void)
{
    type_register_static(&ipod_touch_mipi_dsi_info);
}

type_init(ipod_touch_machine_types)
