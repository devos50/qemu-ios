/*
 * S5L8720 AMC ("Audio Media Codec") hardware audio decoder, as used by AppleAMCDriver_r2 ("AMC 2.0").
 *
 * The AMC is a DSP with 0x30000 bytes of SRAM at 0x22000000 and an internal DMA engine. The driver copies a codec
 * image (MP3, AAC, ALAC or speech) into the SRAM, starts the DSP and then exchanges compressed packets and decoded
 * 16-bit PCM with it through DMA channel 4 and shared structures in the SRAM. The DSP core is not identified, so this
 * model emulates the firmware at a high level instead of running it.
 *
 * Opening a stream (AppleAMCDriver_r2 0xc02b11f4) and flushing one reboot the DSP:
 *  - CLOCK_DIV is set and all interrupts are disabled;
 *  - channels 4 and 5 are halted (CTRL 4, then STATUS bits 2:0 must read 7), their read FIFOs drained and resumed
 *    (CTRL 0x10, then 0x60);
 *  - the boot image is copied into the SRAM by the CPU, DSP_START is written and the driver polls IRQ0_STATUS for
 *    AMC_IRQ_OUTPUT, which it acknowledges through IRQ0_ACK;
 *  - the codec image is copied and DSP_RUN is written 0x100. The driver then reads the output buffer descriptor at
 *    SRAM 0x28000, which the firmware has filled in by then.
 */
#include "hw/arm/ipod_touch_amc.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "exec/address-spaces.h"
#include "qemu/log.h"
#include "trace.h"

static const hwaddr amc_channel_base[AMC_NUM_CHANNELS] = { 0x00, 0x20, 0x40, 0x60, 0x100, 0x180, 0x200, 0x230 };

static int ipod_touch_amc_channel(hwaddr addr, hwaddr *reg)
{
    for (int ch = 0; ch < AMC_NUM_CHANNELS; ch++) {
        hwaddr base = amc_channel_base[ch];
        if (addr >= base && addr < base + 0x20) {
            *reg = addr - base;
            return ch;
        }
    }
    return -1;
}

static void ipod_touch_amc_update_irq(IPodTouchAMCState *s)
{
    bool level = (s->irq_status[0] & s->irq_enabled[0]) || (s->irq_status[1] & s->irq_enabled[1]);
    qemu_set_irq(s->irq, level);
}

static void ipod_touch_amc_raise(IPodTouchAMCState *s, uint32_t bits)
{
    s->irq_status[0] |= bits;
    ipod_touch_amc_update_irq(s);
}

static void ipod_touch_amc_sram_write16(IPodTouchAMCState *s, hwaddr offset, uint16_t val)
{
    uint8_t buf[2] = { val, val >> 8 };
    address_space_write(s->sram_as, s->sram_base + offset, MEMTXATTRS_UNSPECIFIED, buf, sizeof(buf));
}

static void ipod_touch_amc_boot_done(void *opaque)
{
    IPodTouchAMCState *s = opaque;

    trace_ipod_touch_amc_boot_done();
    ipod_touch_amc_raise(s, AMC_IRQ_OUTPUT);
}

/* The codec firmware is running: publish its output buffers. */
static void ipod_touch_amc_dsp_run(IPodTouchAMCState *s)
{
    ipod_touch_amc_sram_write16(s, AMC_SRAM_OUT_DESC + AMC_OUT_NBUF, AMC_OUT_BUFFERS);
    ipod_touch_amc_sram_write16(s, AMC_SRAM_OUT_DESC + AMC_OUT_LEN, AMC_OUT_SAMPLES);
}

static void ipod_touch_amc_channel_ctrl(IPodTouchAMCState *s, int ch, uint32_t cmd)
{
    s->ch_ctrl[ch] = cmd;
    switch (cmd) {
    case AMC_CH_CMD_HALT:
        s->ch_status[ch] = AMC_CH_STATUS_HALTED;
        break;
    case AMC_CH_CMD_RESUME:
    case AMC_CH_CMD_RESET:
        s->ch_status[ch] = 0;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: channel %d command 0x%x\n", __func__, ch, cmd);
        break;
    }
}

static uint64_t ipod_touch_amc_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchAMCState *s = opaque;
    uint64_t val;
    hwaddr reg;
    int ch;

    switch (addr) {
    case AMC_CH4_FIFO_DATA:
    case AMC_CH4_FIFO_COUNT:
    case AMC_CH5_FIFO_DATA:
    case AMC_CH5_FIFO_COUNT:
        // the firmware leaves nothing in the read FIFOs
        val = 0;
        break;
    case AMC_IRQ0_STATUS:
        val = s->irq_status[0] & AMC_IRQ0_MASK;
        break;
    case AMC_IRQ1_STATUS:
        val = s->irq_status[1];
        break;
    case AMC_IRQ0_ENABLE:
    case AMC_IRQ0_DISABLE:
        val = s->irq_enabled[0];
        break;
    case AMC_IRQ1_ENABLE:
    case AMC_IRQ1_DISABLE:
        val = s->irq_enabled[1];
        break;
    default:
        ch = ipod_touch_amc_channel(addr, &reg);
        if (ch >= 0 && reg == AMC_CH_CTRL) {
            val = s->ch_ctrl[ch];
        } else if (ch >= 0 && reg == AMC_CH_STATUS) {
            val = s->ch_status[ch];
        } else {
            val = s->regs[addr / 4];
        }
        break;
    }
    trace_ipod_touch_amc_read(addr, val);
    return val;
}

static void ipod_touch_amc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchAMCState *s = opaque;
    hwaddr reg;
    int ch;

    trace_ipod_touch_amc_write(addr, val);
    switch (addr) {
    case AMC_IRQ0_ENABLE:
        s->irq_enabled[0] |= val & AMC_IRQ0_MASK;
        break;
    case AMC_IRQ0_DISABLE:
        s->irq_enabled[0] &= ~val;
        break;
    case AMC_IRQ0_ACK:
        s->irq_status[0] &= ~(val & AMC_IRQ0_ACK_MASK);
        break;
    case AMC_IRQ1_ENABLE:
        s->irq_enabled[1] |= val;
        break;
    case AMC_IRQ1_DISABLE:
        s->irq_enabled[1] &= ~val;
        break;
    case AMC_DSP_START:
        // the boot image signals that it runs
        timer_mod(s->boot_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + AMC_BOOT_DELAY_NS);
        break;
    case AMC_DSP_RUN:
        ipod_touch_amc_dsp_run(s);
        break;
    case AMC_DOORBELL:
        qemu_log_mask(LOG_UNIMP, "%s: output buffer freed, decoding not implemented\n", __func__);
        break;
    default:
        ch = ipod_touch_amc_channel(addr, &reg);
        if (ch >= 0 && reg == AMC_CH_CTRL) {
            ipod_touch_amc_channel_ctrl(s, ch, val);
        } else if (ch >= 0 && reg == AMC_CH_START) {
            qemu_log_mask(LOG_UNIMP, "%s: channel %d DMA start 0x%08" PRIx64 " not implemented\n", __func__, ch, val);
        } else if (addr != AMC_CLOCK_DIV && addr != AMC_DSP_CONFIG && addr != AMC_CH4_MODE && addr != AMC_CH5_MODE &&
                   !(addr >= AMC_MEM_WINDOWS && addr < AMC_MEM_WINDOWS + 18 * 4)) {
            qemu_log_mask(LOG_UNIMP, "%s: unknown register 0x%04" HWADDR_PRIx " (value 0x%08" PRIx64 ")\n",
                          __func__, addr, val);
        }
        break;
    }
    s->regs[addr / 4] = val;
    ipod_touch_amc_update_irq(s);
}

static const MemoryRegionOps ipod_touch_amc_ops = {
    .read = ipod_touch_amc_read,
    .write = ipod_touch_amc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void ipod_touch_amc_reset(DeviceState *dev)
{
    IPodTouchAMCState *s = IPOD_TOUCH_AMC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->ch_ctrl, 0, sizeof(s->ch_ctrl));
    memset(s->ch_status, 0, sizeof(s->ch_status));
    memset(s->irq_status, 0, sizeof(s->irq_status));
    memset(s->irq_enabled, 0, sizeof(s->irq_enabled));
    timer_del(s->boot_timer);
    qemu_irq_lower(s->irq);
}

static void ipod_touch_amc_init(Object *obj)
{
    IPodTouchAMCState *s = IPOD_TOUCH_AMC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &ipod_touch_amc_ops, s, TYPE_IPOD_TOUCH_AMC, AMC_REG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->boot_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ipod_touch_amc_boot_done, s);
    s->sram_as = &address_space_memory;
}

static void ipod_touch_amc_finalize(Object *obj)
{
    IPodTouchAMCState *s = IPOD_TOUCH_AMC(obj);
    timer_free(s->boot_timer);
}

static Property ipod_touch_amc_properties[] = {
    DEFINE_PROP_UINT64("sram-base", IPodTouchAMCState, sram_base, 0x22000000),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_ipod_touch_amc = {
    .name = TYPE_IPOD_TOUCH_AMC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IPodTouchAMCState, AMC_REG_SIZE / 4),
        VMSTATE_UINT32_ARRAY(ch_ctrl, IPodTouchAMCState, AMC_NUM_CHANNELS),
        VMSTATE_UINT32_ARRAY(ch_status, IPodTouchAMCState, AMC_NUM_CHANNELS),
        VMSTATE_UINT32_ARRAY(irq_status, IPodTouchAMCState, 2),
        VMSTATE_UINT32_ARRAY(irq_enabled, IPodTouchAMCState, 2),
        VMSTATE_TIMER_PTR(boot_timer, IPodTouchAMCState),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_amc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = ipod_touch_amc_reset;
    dc->vmsd = &vmstate_ipod_touch_amc;
    device_class_set_props(dc, ipod_touch_amc_properties);
}

static const TypeInfo ipod_touch_amc_type_info = {
    .name = TYPE_IPOD_TOUCH_AMC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchAMCState),
    .instance_init = ipod_touch_amc_init,
    .instance_finalize = ipod_touch_amc_finalize,
    .class_init = ipod_touch_amc_class_init,
};

static void ipod_touch_amc_register_types(void)
{
    type_register_static(&ipod_touch_amc_type_info);
}

type_init(ipod_touch_amc_register_types)
