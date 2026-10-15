/*
 * S5L8900 SPI Emulation
 *
 * by cmw
 */

#include "hw/arm/s5l_spi.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

/*
 * Without a TX count (the S5L8900), the controller sends what is queued while it runs, a moment later as the bus
 * would, and TX empty is an event the driver acknowledges, not a level: iBoot's interrupt handler refills the FIFO
 * and then clears the status bits it saw.
 */
#define S5L_SPI_XFER_DELAY_NS   10000

static int apple_spi_word_size(S5LSPIState *s)
{
    switch ((REG(s, R_CFG) >> s->word_size_shift) & 0x3) {
    case R_CFG_WORD_SIZE_8B:
        return 1;
    case R_CFG_WORD_SIZE_16B:
        return 2;
    case R_CFG_WORD_SIZE_32B:
        return 4;
    default:
        break;
    }
    g_assert_not_reached();
}

static void apple_spi_update_xfer_tx(S5LSPIState *s)
{
    if (s->tx_count && fifo8_is_empty(&s->tx_fifo)) {
        REG(s, R_STATUS) |= R_STATUS_TXEMPTY;
    }
}

static void apple_spi_update_xfer_rx(S5LSPIState *s)
{
    if (!fifo8_is_empty(&s->rx_fifo)) {
        REG(s, R_STATUS) |= R_STATUS_RXREADY;
    }
}

static void apple_spi_update_irq(S5LSPIState *s)
{
    uint32_t irq = 0;
    uint32_t mask = 0;

    if (REG(s, R_CFG) & R_CFG_IE_RXREADY) {
        mask |= R_STATUS_RXREADY;
    }
    if (REG(s, R_CFG) & R_CFG_IE_TXEMPTY) {
        mask |= R_STATUS_TXEMPTY;
    }
    if (REG(s, R_CFG) & R_CFG_IE_COMPLETE) {
        mask |= R_STATUS_COMPLETE;
    }

    if (REG(s, R_STATUS) & mask) {
        irq = 1;
    }
    if (irq != s->last_irq) {
        s->last_irq = irq;
        qemu_set_irq(s->irq, irq);
    }
}

static void apple_spi_update_cs(S5LSPIState *s)
{
    BusState *b = BUS(s->spi);
    BusChild *kid = QTAILQ_FIRST(&b->children);
    if (kid) {
        // TODO GPIO not properly setup yet
        //qemu_set_irq(qdev_get_gpio_in_named(kid->child, SSI_GPIO_CS, 0), (REG(s, R_PIN) & R_PIN_CS) != 0);
    }
}

static void apple_spi_cs_set(void *opaque, int pin, int level)
{
    S5LSPIState *s = S5L_SPI(opaque);
    if (level) {
        REG(s, R_PIN) |= R_PIN_CS;
    } else {
        REG(s, R_PIN) &= ~R_PIN_CS;
    }
    apple_spi_update_cs(s);
}

static void apple_spi_xfer(S5LSPIState *s);

static void apple_spi_run(S5LSPIState *s)
{
    if (!(REG(s, R_CTRL) & R_CTRL_RUN)) {
        return;
    }
    if (s->tx_count) {
        if (REG(s, R_RXCNT) || REG(s, R_TXCNT)) {
            apple_spi_xfer(s);
        }
    } else if (!fifo8_is_empty(&s->tx_fifo) || (REG(s, R_RXCNT) && (REG(s, R_CFG) & R_CFG_AGD))) {
        if (!timer_pending(s->xfer_timer)) {
            timer_mod(s->xfer_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + S5L_SPI_XFER_DELAY_NS);
        }
    }
}

static void apple_spi_xfer(S5LSPIState *s)
{
    uint32_t tx;
    uint32_t rx;
    bool sent = false;

    apple_spi_update_xfer_tx(s);

    //printf("TX queue: %d, RX queue: %d\n", REG(s, R_TXCNT), REG(s, R_RXCNT));
    //printf("TX buffer size: %d, RX buffer size: %d\n", fifo8_num_used(&s->tx_fifo), fifo8_num_used(&s->rx_fifo));

    while ((REG(s, R_TXCNT) || !s->tx_count) && !fifo8_is_empty(&s->tx_fifo)) {
        tx = (uint32_t)fifo8_pop(&s->tx_fifo);
        rx = ssi_transfer(s->spi, tx);
        sent = true;
        if (s->tx_count) {
            REG(s, R_TXCNT)--;
        }
        apple_spi_update_xfer_tx(s);
        if (REG(s, R_RXCNT) > 0) {
            if (fifo8_is_full(&s->rx_fifo)) {
                hw_error("Rx buffer overflow: %d\n", fifo8_num_free(&s->rx_fifo));
                qemu_log_mask(LOG_GUEST_ERROR, "%s: rx overflow\n", __func__);
                REG(s, R_STATUS) |= R_STATUS_RXOVERFLOW;
            } else {
                fifo8_push(&s->rx_fifo, (uint8_t)rx);
                REG(s, R_RXCNT)--;
                apple_spi_update_xfer_rx(s);
            }
        }
    }

    // fetch the remaining bytes by sending sentinel bytes.
    while (!fifo8_is_full(&s->rx_fifo) && (REG(s, R_RXCNT) > 0) && (REG(s, R_CFG) & R_CFG_AGD)) {
        rx = ssi_transfer(s->spi, 0xff);
        if (fifo8_is_full(&s->rx_fifo)) {
            hw_error("Rx buffer overflow: %d\n", fifo8_num_free(&s->rx_fifo));
            qemu_log_mask(LOG_GUEST_ERROR, "%s: rx overflow\n", __func__);
            REG(s, R_STATUS) |= R_STATUS_RXOVERFLOW;
            break;
        } else {
            fifo8_push(&s->rx_fifo, (uint8_t)rx);
            REG(s, R_RXCNT)--;
            apple_spi_update_xfer_rx(s);
        }
    }
    if (sent && !s->tx_count) {
        REG(s, R_STATUS) |= R_STATUS_TXEMPTY;
    }
    if (REG(s, R_RXCNT) == 0 && (s->tx_count ? REG(s, R_TXCNT) == 0 : fifo8_is_empty(&s->tx_fifo))) {
        REG(s, R_STATUS) |= R_STATUS_COMPLETE;
    }

    //printf("<after> TX buffer size: %d, RX buffer size: %d\n", fifo8_num_used(&s->tx_fifo), fifo8_num_used(&s->rx_fifo));
}

static void apple_spi_xfer_timer(void *opaque)
{
    S5LSPIState *s = S5L_SPI(opaque);

    if (REG(s, R_CTRL) & R_CTRL_RUN) {
        apple_spi_xfer(s);
    }
    apple_spi_update_irq(s);
}

static uint64_t s5l_spi_read(void *opaque, hwaddr addr, unsigned size)
{
    S5LSPIState *s = S5L_SPI(opaque);
    //printf("%s (base %d): read from location 0x%08x\n", __func__, s->base, addr);

    uint32_t r;
    bool run = false;

    r = s->regs[addr >> 2];
    switch (addr) {
        case R_RXDATA: {
            const uint8_t *buf = NULL;
            int word_size = apple_spi_word_size(s);
            uint32_t num = 0;
            if (fifo8_is_empty(&s->rx_fifo)) {
                hw_error("Rx buffer underflow\n");
                qemu_log_mask(LOG_GUEST_ERROR, "%s: rx underflow\n", __func__);
                r = 0;
                break;
            }
            buf = fifo8_pop_bufptr(&s->rx_fifo, word_size, &num);
            memcpy(&r, buf, num);

            if (fifo8_is_empty(&s->rx_fifo)) {
                run = true;
            }
            break;
        }
        case R_STATUS: {
            uint32_t tx_level = fifo8_num_used(&s->tx_fifo);
            uint32_t rx_level = fifo8_num_used(&s->rx_fifo);
            if (s->level_width) {
                uint32_t mask = (1u << s->level_width) - 1;
                r &= ~((mask << s->tx_level_shift) | (mask << s->rx_level_shift));
                tx_level = MIN(tx_level, mask);
                rx_level = MIN(rx_level, mask);
            }
            r |= (tx_level << s->tx_level_shift) | (rx_level << s->rx_level_shift);
            break;
        }
        default:
            break;
    }

    if (run) {
        apple_spi_run(s);
    }
    apple_spi_update_irq(s);
    return r;
}

static void s5l_spi_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    S5LSPIState *s = S5L_SPI(opaque);
    //printf("%s (base %d): writing 0x%08x to 0x%08x\n", __func__, s->base, data, addr);

    uint32_t r = data;
    uint32_t *mmio = &REG(s, addr);
    uint32_t old = *mmio;
    bool cs_flg = false;
    bool run = false;

    switch (addr) {
    case R_CTRL:
        if (r & R_CTRL_TX_RESET) {
            fifo8_reset(&s->tx_fifo);
        }
        if (r & R_CTRL_RX_RESET) {
            fifo8_reset(&s->rx_fifo);
        }
        if (r & R_CTRL_RUN && !fifo8_is_empty(&s->tx_fifo)) {
            run = true;
        }
        break;
    case R_STATUS:
        run = true;
        r = old & (~r);
        break;
    case R_PIN:
        cs_flg = true;
        break;
    case R_TXDATA ... R_TXDATA + 3: {
        int word_size = apple_spi_word_size(s);
        if (fifo8_is_full(&s->tx_fifo) || fifo8_num_free(&s->tx_fifo) < word_size) {
            hw_error("Tx buffer overflow: %d\n", fifo8_num_free(&s->tx_fifo));
        }
        fifo8_push_all(&s->tx_fifo, (uint8_t *)&r, word_size);
        run = !s->tx_count;
        break;
    case R_CFG:
        run = true;
        break;
    }
    default:
        break;
    }

    *mmio = r;
    if (cs_flg) {
        apple_spi_update_cs(s);
    }
    if (run) {
        apple_spi_run(s);
    }

    if(addr == R_STATUS) {
        apple_spi_update_xfer_tx(s);
        apple_spi_update_xfer_rx(s);
    }

    apple_spi_update_irq(s);
}

static const MemoryRegionOps spi_ops = {
    .read = s5l_spi_read,
    .write = s5l_spi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void s5l_spi_reset(DeviceState *d)
{
    S5LSPIState *s = (S5LSPIState *)d;
	memset(s->regs, 0, sizeof(s->regs));
    timer_del(s->xfer_timer);
    fifo8_reset(&s->tx_fifo);
    fifo8_reset(&s->rx_fifo);
}

static uint32_t base_addr = 0;

void set_spi_base(uint32_t base)
{
	base_addr = base;
}

static void s5l_spi_realize(DeviceState *dev, struct Error **errp)
{
    S5LSPIState *s = S5L_SPI(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    char bus_name[32] = { 0 };
    snprintf(bus_name, sizeof(bus_name), "%s.bus", dev->id);
    s->spi = ssi_create_bus(dev, (const char *)bus_name);

    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->cs_line);
    qdev_init_gpio_in_named(dev, apple_spi_cs_set, SSI_GPIO_CS, 1);
    char name[5];
    snprintf(name, 5, "spi%d", base_addr);
    memory_region_init_io(&s->iomem, OBJECT(s), &spi_ops, s, name, 0x100);
    sysbus_init_mmio(sbd, &s->iomem);
    s->base = base_addr;

    fifo8_create(&s->tx_fifo, R_FIFO_TX_DEPTH);
    fifo8_create(&s->rx_fifo, s->rx_fifo_depth);
    s->xfer_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, apple_spi_xfer_timer, s);
}

static bool s5l_spi_xfer_timer_needed(void *opaque)
{
    return !S5L_SPI(opaque)->tx_count;
}

static const VMStateDescription vmstate_s5l_spi_xfer_timer = {
    .name = TYPE_S5L_SPI "/xfer-timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = s5l_spi_xfer_timer_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(xfer_timer, S5LSPIState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_s5l_spi = {
    .name = TYPE_S5L_SPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(last_irq, S5LSPIState),
        VMSTATE_UINT32_ARRAY(regs, S5LSPIState, MMIO_SIZE >> 2),
        VMSTATE_FIFO8(rx_fifo, S5LSPIState),
        VMSTATE_FIFO8(tx_fifo, S5LSPIState),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_s5l_spi_xfer_timer,
        NULL
    }
};

static const Property s5l_spi_properties[] = {
    DEFINE_PROP_UINT8("cfg-word-size-shift", S5LSPIState, word_size_shift, R_CFG_WORD_SIZE_SHIFT),
    DEFINE_PROP_UINT8("status-tx-level-shift", S5LSPIState, tx_level_shift, R_STATUS_TXFIFO_SHIFT),
    DEFINE_PROP_UINT8("status-rx-level-shift", S5LSPIState, rx_level_shift, R_STATUS_RXFIFO_SHIFT),
    // the width of the FIFO level fields, 0 for levels ORed in unclipped
    DEFINE_PROP_UINT8("status-level-width", S5LSPIState, level_width, 0),
    DEFINE_PROP_UINT32("rx-fifo-depth", S5LSPIState, rx_fifo_depth, R_FIFO_RX_DEPTH),
    DEFINE_PROP_BOOL("tx-count", S5LSPIState, tx_count, true),
};

static void s5l_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = s5l_spi_realize;
    dc->vmsd = &vmstate_s5l_spi;
    device_class_set_legacy_reset(dc, s5l_spi_reset);
    device_class_set_props(dc, s5l_spi_properties);
}

static const TypeInfo s5l_spi_info = {
    .name          = TYPE_S5L_SPI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5LSPIState),
    .class_init    = s5l_spi_class_init,
};

static void s5l_spi_register_types(void)
{
    type_register_static(&s5l_spi_info);
}

type_init(s5l_spi_register_types)