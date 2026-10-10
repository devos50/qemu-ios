/*
 * S5L8720 SDIO host controller (AppleS5L8900XSDIO), with the BCM4325 Wi-Fi card in its slot.
 *
 * The driver issues a command by writing the argument and the command (index, response type, bit 8 for a CMD53
 * write), and then setting the start bit; it polls DSTA for the command-ready and done bits. A CMD53 moves its data by
 * DMA: the driver programs the address and block geometry before the command and starts the DMA through DCTRL
 * afterwards, then waits for the data-done interrupt (or polls for it). The transfer itself happens when the command
 * executes; the interrupt is raised once the DMA is started.
 *
 * The card interrupt follows the card's level. On the S5L8720 the driver acknowledges it and then toggles CSR bit 1 to
 * re-arm it, which latches it again if the card still asserts it.
 */
#include "hw/arm/ipod_touch_sdio.h"
#include "hw/qdev-properties.h"
#include "exec/address-spaces.h"
#include "qemu/log.h"
#include "trace.h"

static void ipod_touch_sdio_update_irq(IPodTouchSDIOState *s)
{
    qemu_set_irq(s->irq, (s->irq_status & s->irq_mask) != 0);
}

static void ipod_touch_sdio_card_irq(void *opaque, bool level)
{
    IPodTouchSDIOState *s = opaque;

    s->card_irq = level;
    if (level) {
        s->irq_status |= SDIO_IRQ_CARD;
    } else {
        s->irq_status &= ~SDIO_IRQ_CARD;
    }
    ipod_touch_sdio_update_irq(s);
}

static void ipod_touch_sdio_transfer(IPodTouchSDIOState *s)
{
    bool write = s->arg & SDIO_ARG_WRITE;
    uint32_t blklen = s->blklen ? s->blklen : 512;
    uint32_t len = blklen * MAX(s->numblk, 1);
    g_autofree uint8_t *buf = g_malloc0(len);

    if (write) {
        address_space_read(&address_space_memory, s->baddr, MEMTXATTRS_UNSPECIFIED, buf, len);
    }
    bcm4325_io_rw_extended(&s->card, s->arg, buf, len);
    if (!write) {
        address_space_write(&address_space_memory, s->baddr, MEMTXATTRS_UNSPECIFIED, buf, len);
    }
    s->data_done = true;
}

static void ipod_touch_sdio_exec_cmd(IPodTouchSDIOState *s)
{
    uint8_t index = s->cmd & SDIO_CMD_INDEX;
    uint32_t resp = 0;
    bool ok = bcm4325_command(&s->card, index, s->arg, &resp);

    trace_ipod_touch_sdio_cmd(index, s->arg, ok, resp);
    s->cmd &= ~SDIO_CMD_START;
    memset(s->resp, 0, sizeof(s->resp));
    s->resp[0] = resp;
    s->dsta = SDIO_DSTA_CMD_RDY | SDIO_DSTA_CMD_DONE | (ok ? 0 : SDIO_DSTA_TIMEOUT);
    if (ok && index == SD_CMD_IO_RW_EXTENDED) {
        ipod_touch_sdio_transfer(s);
    }
}

static void ipod_touch_sdio_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    IPodTouchSDIOState *s = opaque;

    switch (addr) {
    case SDIO_CTRL:
        s->ctrl = value;
        break;
    case SDIO_DCTRL:
        s->dctrl = value;
        if ((value & SDIO_DCTRL_START) && s->data_done) {
            s->data_done = false;
            s->irq_status |= SDIO_IRQ_DAT_DONE;
            ipod_touch_sdio_update_irq(s);
        }
        break;
    case SDIO_CMD:
        s->cmd = value;
        if (value & SDIO_CMD_START) {
            ipod_touch_sdio_exec_cmd(s);
        }
        break;
    case SDIO_ARGU:
        s->arg = value;
        break;
    case SDIO_STATE:
        break;
    case SDIO_STAC:
        s->stac = value;
        break;
    case SDIO_CLKDIV:
        s->clkdiv = value;
        break;
    case SDIO_CSR: {
        uint32_t old = s->csr;
        s->csr = value;
        if (!(old & SDIO_CSR_CARD_INT) && (value & SDIO_CSR_CARD_INT) && s->card_irq) {
            s->irq_status |= SDIO_IRQ_CARD;
            ipod_touch_sdio_update_irq(s);
        }
        break;
    }
    case SDIO_IRQ:
        s->irq_status &= ~value;
        ipod_touch_sdio_update_irq(s);
        break;
    case SDIO_IRQMASK:
        s->irq_mask = value;
        ipod_touch_sdio_update_irq(s);
        break;
    case SDIO_BADDR:
        s->baddr = value;
        break;
    case SDIO_BLKLEN:
        s->blklen = value;
        break;
    case SDIO_NUMBLK:
        s->numblk = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unknown register 0x%02" HWADDR_PRIx " (value 0x%08" PRIx64 ")\n",
                      __func__, addr, value);
        break;
    }
}

static uint64_t ipod_touch_sdio_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchSDIOState *s = opaque;

    switch (addr) {
    case SDIO_CTRL:
        return s->ctrl;
    case SDIO_DCTRL:
        return s->dctrl;
    case SDIO_CMD:
        return s->cmd;
    case SDIO_ARGU:
        return s->arg;
    case SDIO_STATE:
        return 0; // never busy
    case SDIO_STAC:
        return s->stac;
    case SDIO_DSTA:
        return s->dsta | SDIO_DSTA_CMD_RDY;
    case SDIO_FSTA:
        return 0;
    case SDIO_RESP0:
    case SDIO_RESP1:
    case SDIO_RESP2:
    case SDIO_RESP3:
        return s->resp[(addr - SDIO_RESP0) / 4];
    case SDIO_CLKDIV:
        return s->clkdiv;
    case SDIO_CSR:
        return s->csr;
    case SDIO_IRQ:
        return s->irq_status;
    case SDIO_IRQMASK:
        return s->irq_mask;
    case SDIO_BADDR:
        return s->baddr;
    case SDIO_BLKLEN:
        return s->blklen;
    case SDIO_NUMBLK:
        return s->numblk;
    case SDIO_REMBLK:
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unknown register 0x%02" HWADDR_PRIx "\n", __func__, addr);
        return 0;
    }
}

static const MemoryRegionOps ipod_touch_sdio_ops = {
    .read = ipod_touch_sdio_read,
    .write = ipod_touch_sdio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_sdio_reset(DeviceState *dev)
{
    IPodTouchSDIOState *s = IPOD_TOUCH_SDIO(dev);

    s->ctrl = 0;
    s->dctrl = 0;
    s->cmd = 0;
    s->arg = 0;
    s->stac = 0;
    s->dsta = 0;
    memset(s->resp, 0, sizeof(s->resp));
    s->clkdiv = 0;
    s->csr = 0;
    s->irq_status = 0;
    s->irq_mask = 0;
    s->baddr = 0;
    s->blklen = 0;
    s->numblk = 0;
    s->data_done = false;
    bcm4325_reset(&s->card);
    ipod_touch_sdio_update_irq(s);
}

static void ipod_touch_sdio_init(Object *obj)
{
    IPodTouchSDIOState *s = IPOD_TOUCH_SDIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &ipod_touch_sdio_ops, s, TYPE_IPOD_TOUCH_SDIO, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void ipod_touch_sdio_realize(DeviceState *dev, Error **errp)
{
    IPodTouchSDIOState *s = IPOD_TOUCH_SDIO(dev);

    bcm4325_init(&s->card, dev, ipod_touch_sdio_card_irq, s);
}

static Property ipod_touch_sdio_properties[] = {
    DEFINE_NIC_PROPERTIES(IPodTouchSDIOState, card.conf),
    DEFINE_PROP_END_OF_LIST(),
};

static void ipod_touch_sdio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ipod_touch_sdio_realize;
    dc->reset = ipod_touch_sdio_reset;
    device_class_set_props(dc, ipod_touch_sdio_properties);
}

static const TypeInfo ipod_touch_sdio_type_info = {
    .name = TYPE_IPOD_TOUCH_SDIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchSDIOState),
    .instance_init = ipod_touch_sdio_init,
    .class_init = ipod_touch_sdio_class_init,
};

static void ipod_touch_sdio_register_types(void)
{
    type_register_static(&ipod_touch_sdio_type_info);
}

type_init(ipod_touch_sdio_register_types)
