/*
 * S5L8720 I2S controller (i2s0), as programmed by AppleS5L8900XI2SController.
 *
 * The driver configures CLKCON, 0x40, TXCON, RXCON, clears TXCOM/RXCOM and writes 1 to STATUS, then per stream starts
 * a DMA transfer on DMAC0 (TX: peripheral 10 into TXFIFO, RX: peripheral 11 from RXFIFO, 16-bit) and writes 6 to
 * TXCOM/RXCOM; 0 stops. With TXCON bit 20 set (iOS uses TXCON 0x03100019) it first enables the i2s0 interrupt, GPIO
 * interrupt 0x2c, and waits for it; the handler only masks it again. We take this to be a frame sync (LRCK) edge and
 * raise it once per tick while the clock is enabled. The other TXCON/RXCON bits are not interpreted: the format is
 * 16-bit stereo, as the DMA and the audio device use.
 *
 * The transmitter consumes samples at the sample rate in virtual time: every tick, it requests the frames that are
 * due from the DMA and fills frames the DMA did not provide with silence. The frames go to a ring that the host audio
 * output drains. The receiver provides silence, as no microphone is modelled.
 */
#include "hw/arm/ipod_touch_i2s.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "trace.h"

static bool ipod_touch_i2s_tx_running(IPodTouchI2SState *s)
{
    return s->txcom != 0;
}

static bool ipod_touch_i2s_rx_running(IPodTouchI2SState *s)
{
    return s->rxcom != 0;
}

static void ipod_touch_i2s_update_timer(IPodTouchI2SState *s)
{
    if ((s->clkcon & I2S_CLKCON_ENABLE) || ipod_touch_i2s_tx_running(s) || ipod_touch_i2s_rx_running(s)) {
        if (!timer_pending(s->timer)) {
            timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + I2S_TICK_NS);
        }
    } else {
        timer_del(s->timer);
    }
}

static void ipod_touch_i2s_push_frame(IPodTouchI2SState *s, int16_t left, int16_t right)
{
    if (!s->voice) {
        return;
    }
    if (s->out_fill == I2S_OUT_FRAMES) {
        // the host output falls behind: drop the oldest frame
        trace_ipod_touch_i2s_out_overrun();
        s->out_fill--;
    } else if (s->out_fill == 0) {
        AUD_set_active_out(s->voice, 1);
    }
    s->out[s->out_head][0] = left;
    s->out[s->out_head][1] = right;
    s->out_head = (s->out_head + 1) % I2S_OUT_FRAMES;
    s->out_fill++;
}

static void ipod_touch_i2s_audio_out(void *opaque, int free)
{
    IPodTouchI2SState *s = opaque;
    uint32_t frames = MIN(s->out_fill, free / sizeof(s->out[0]));

    while (frames > 0) {
        uint32_t tail = (s->out_head + I2S_OUT_FRAMES - s->out_fill) % I2S_OUT_FRAMES;
        uint32_t chunk = MIN(frames, I2S_OUT_FRAMES - tail);
        size_t written = AUD_write(s->voice, s->out[tail], chunk * sizeof(s->out[0])) / sizeof(s->out[0]);
        s->out_fill -= written;
        frames -= written;
        if (written < chunk) {
            break;
        }
    }
    if (s->out_fill == 0 && !ipod_touch_i2s_tx_running(s)) {
        AUD_set_active_out(s->voice, 0);
    }
}

static void ipod_touch_i2s_open_voice(IPodTouchI2SState *s)
{
    struct audsettings as = {
        .freq = s->sample_rate,
        .nchannels = 2,
        .fmt = AUDIO_FORMAT_S16,
        .endianness = 0,
    };

    if (!s->card.state) {
        return;
    }
    s->voice = AUD_open_out(&s->card, s->voice, TYPE_IPOD_TOUCH_I2S, s, ipod_touch_i2s_audio_out, &as);
    s->out_fill = 0;
}

void ipod_touch_i2s_set_sample_rate(IPodTouchI2SState *s, uint32_t rate)
{
    if (rate == 0 || rate == s->sample_rate) {
        return;
    }
    s->sample_rate = rate;
    s->tx_start_ns = s->rx_start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->tx_frames = s->rx_frames = 0;
    ipod_touch_i2s_open_voice(s);
}

// Frames that became due since the stream started, limited to I2S_MAX_CATCHUP worth.
static uint32_t ipod_touch_i2s_due(IPodTouchI2SState *s, int64_t now, int64_t start_ns, uint64_t *done)
{
    uint64_t due = muldiv64(now - start_ns, s->sample_rate, NANOSECONDS_PER_SECOND);
    uint64_t max = muldiv64(I2S_MAX_CATCHUP, s->sample_rate, NANOSECONDS_PER_SECOND);

    if (due - *done > max) {
        *done = due - max;
    }
    uint32_t n = due - *done;
    *done = due;
    return n;
}

static void ipod_touch_i2s_tick(void *opaque)
{
    IPodTouchI2SState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->clkcon & I2S_CLKCON_ENABLE) {
        qemu_irq_pulse(s->sync_irq);
    }

    if (ipod_touch_i2s_tx_running(s)) {
        uint32_t frames = ipod_touch_i2s_due(s, now, s->tx_start_ns, &s->tx_frames);

        // The DMA writes the samples into TXFIFO while the request is asserted, from within qemu_set_irq.
        s->tx_room = frames * 2;
        if (s->tx_room) {
            qemu_irq_raise(s->tx_dreq);
        }
        if (s->tx_room) {
            trace_ipod_touch_i2s_tx_underrun(frames, s->tx_room);
            qemu_irq_lower(s->tx_dreq);
            for (uint32_t i = 0; i < s->tx_room / 2; i++) {
                ipod_touch_i2s_push_frame(s, 0, 0);
            }
            s->tx_room = 0;
        }
    }

    if (ipod_touch_i2s_rx_running(s)) {
        uint32_t frames = ipod_touch_i2s_due(s, now, s->rx_start_ns, &s->rx_frames);

        s->rx_avail = frames * 2;
        if (s->rx_avail) {
            qemu_irq_raise(s->rx_dreq);
        }
        if (s->rx_avail) {
            qemu_irq_lower(s->rx_dreq);
            s->rx_avail = 0;
        }
    }

    ipod_touch_i2s_update_timer(s);
}

static void ipod_touch_i2s_tx_sample(IPodTouchI2SState *s, uint16_t sample)
{
    if (s->tx_room == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: TX FIFO overflow\n", __func__);
        return;
    }
    s->frame[s->tx_sample++] = sample;
    if (s->tx_sample == 2) {
        ipod_touch_i2s_push_frame(s, s->frame[0], s->frame[1]);
        s->tx_sample = 0;
    }
    if (--s->tx_room == 0) {
        qemu_irq_lower(s->tx_dreq);
    }
}

static uint16_t ipod_touch_i2s_rx_sample(IPodTouchI2SState *s)
{
    if (s->rx_avail == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: RX FIFO underflow\n", __func__);
        return 0;
    }
    if (--s->rx_avail == 0) {
        qemu_irq_lower(s->rx_dreq);
    }
    return 0;
}

static void ipod_touch_i2s_start(int64_t *start_ns, uint64_t *frames)
{
    *start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    *frames = 0;
}

static uint64_t ipod_touch_i2s_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchI2SState *s = opaque;
    uint64_t val;

    switch (addr) {
    case I2S_CLKCON: val = s->clkcon; break;
    case I2S_TXCON:  val = s->txcon; break;
    case I2S_TXCOM:  val = s->txcom; break;
    case I2S_RXCON:  val = s->rxcon; break;
    case I2S_RXCOM:  val = s->rxcom; break;
    case I2S_STATUS: val = s->status; break;
    case I2S_CFG40:  val = s->cfg40; break;
    case I2S_RXFIFO:
        val = ipod_touch_i2s_rx_sample(s);
        if (size == 4) {
            val |= (uint32_t)ipod_touch_i2s_rx_sample(s) << 16;
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unknown register 0x%02" HWADDR_PRIx "\n", __func__, addr);
        val = 0;
        break;
    }
    if (addr != I2S_RXFIFO) {
        trace_ipod_touch_i2s_read(addr, val);
    }
    return val;
}

static void ipod_touch_i2s_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchI2SState *s = opaque;

    if (addr == I2S_TXFIFO) {
        ipod_touch_i2s_tx_sample(s, val);
        if (size == 4) {
            ipod_touch_i2s_tx_sample(s, val >> 16);
        }
        return;
    }

    trace_ipod_touch_i2s_write(addr, val);
    switch (addr) {
    case I2S_CLKCON:
        s->clkcon = val;
        break;
    case I2S_TXCON:
        s->txcon = val;
        break;
    case I2S_TXCOM:
        if (val != 0 && val != I2S_COM_START) {
            qemu_log_mask(LOG_UNIMP, "%s: TXCOM 0x%" PRIx64 " treated as start\n", __func__, val);
        }
        if (val && !ipod_touch_i2s_tx_running(s)) {
            ipod_touch_i2s_start(&s->tx_start_ns, &s->tx_frames);
            s->tx_sample = 0;
        } else if (!val && ipod_touch_i2s_tx_running(s)) {
            trace_ipod_touch_i2s_tx_stop(s->tx_frames, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->tx_start_ns);
        }
        s->txcom = val;
        break;
    case I2S_RXCON:
        s->rxcon = val;
        break;
    case I2S_RXCOM:
        if (val != 0 && val != I2S_COM_START) {
            qemu_log_mask(LOG_UNIMP, "%s: RXCOM 0x%" PRIx64 " treated as start\n", __func__, val);
        }
        if (val && !ipod_touch_i2s_rx_running(s)) {
            ipod_touch_i2s_start(&s->rx_start_ns, &s->rx_frames);
        }
        s->rxcom = val;
        break;
    case I2S_STATUS:
        s->status = val;
        break;
    case I2S_CFG40:
        s->cfg40 = val;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unknown register 0x%02" HWADDR_PRIx " (value 0x%08" PRIx64 ")\n",
                      __func__, addr, val);
        break;
    }
    ipod_touch_i2s_update_timer(s);
}

static const MemoryRegionOps ipod_touch_i2s_ops = {
    .read = ipod_touch_i2s_read,
    .write = ipod_touch_i2s_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static void ipod_touch_i2s_reset(DeviceState *dev)
{
    IPodTouchI2SState *s = IPOD_TOUCH_I2S(dev);

    s->clkcon = s->txcon = s->txcom = s->rxcon = s->rxcom = s->status = s->cfg40 = 0;
    s->tx_room = s->rx_avail = s->tx_sample = 0;
    s->out_fill = 0;
    timer_del(s->timer);
    qemu_irq_lower(s->tx_dreq);
    qemu_irq_lower(s->rx_dreq);
    if (s->voice) {
        AUD_set_active_out(s->voice, 0);
    }
}

static void ipod_touch_i2s_init(Object *obj)
{
    IPodTouchI2SState *s = IPOD_TOUCH_I2S(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &ipod_touch_i2s_ops, s, TYPE_IPOD_TOUCH_I2S, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    qdev_init_gpio_out_named(DEVICE(obj), &s->sync_irq, "sync", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->tx_dreq, "dma-tx", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->rx_dreq, "dma-rx", 1);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ipod_touch_i2s_tick, s);
}

static void ipod_touch_i2s_realize(DeviceState *dev, Error **errp)
{
    IPodTouchI2SState *s = IPOD_TOUCH_I2S(dev);

    // Without an audiodev, the samples are consumed at the same rate but not played.
    if (s->card.state) {
        if (!AUD_register_card(TYPE_IPOD_TOUCH_I2S, &s->card, errp)) {
            return;
        }
        ipod_touch_i2s_open_voice(s);
    }
}

static void ipod_touch_i2s_finalize(Object *obj)
{
    IPodTouchI2SState *s = IPOD_TOUCH_I2S(obj);
    timer_free(s->timer);
}

static Property ipod_touch_i2s_properties[] = {
    DEFINE_AUDIO_PROPERTIES(IPodTouchI2SState, card),
    DEFINE_PROP_UINT32("sample-rate", IPodTouchI2SState, sample_rate, 44100),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_ipod_touch_i2s = {
    .name = TYPE_IPOD_TOUCH_I2S,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(clkcon, IPodTouchI2SState),
        VMSTATE_UINT32(txcon, IPodTouchI2SState),
        VMSTATE_UINT32(txcom, IPodTouchI2SState),
        VMSTATE_UINT32(rxcon, IPodTouchI2SState),
        VMSTATE_UINT32(rxcom, IPodTouchI2SState),
        VMSTATE_UINT32(status, IPodTouchI2SState),
        VMSTATE_UINT32(cfg40, IPodTouchI2SState),
        VMSTATE_UINT32(sample_rate, IPodTouchI2SState),
        VMSTATE_INT64(tx_start_ns, IPodTouchI2SState),
        VMSTATE_UINT64(tx_frames, IPodTouchI2SState),
        VMSTATE_UINT32(tx_sample, IPodTouchI2SState),
        VMSTATE_INT64(rx_start_ns, IPodTouchI2SState),
        VMSTATE_UINT64(rx_frames, IPodTouchI2SState),
        VMSTATE_TIMER_PTR(timer, IPodTouchI2SState),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_i2s_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ipod_touch_i2s_realize;
    dc->reset = ipod_touch_i2s_reset;
    dc->vmsd = &vmstate_ipod_touch_i2s;
    device_class_set_props(dc, ipod_touch_i2s_properties);
}

static const TypeInfo ipod_touch_i2s_type_info = {
    .name = TYPE_IPOD_TOUCH_I2S,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchI2SState),
    .instance_init = ipod_touch_i2s_init,
    .instance_finalize = ipod_touch_i2s_finalize,
    .class_init = ipod_touch_i2s_class_init,
};

static void ipod_touch_i2s_register_types(void)
{
    type_register_static(&ipod_touch_i2s_type_info);
}

type_init(ipod_touch_i2s_register_types)
