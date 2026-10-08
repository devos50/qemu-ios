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
 *
 * Compressed data arrives through channel 4 (c02ae2d8): the driver builds a chain of descriptors in DRAM, enables
 * AMC_IRQ_CH4_DONE and writes the first descriptor's address to the channel's START register. The handler masks the
 * interrupt again, acknowledges the transfer with CTRL 0x20 and returns the buffer to userland. The data is the codec's
 * raw packets back to back (for AAC the raw_data_blocks of the MP4 file) without their sizes: the firmware finds the
 * frame boundaries while decoding. Transfers complete right away: the driver starts the next one whenever userland
 * queues input and asserts that the channel is idle, and userland only queues input as it needs output.
 *
 * Decoded 16-bit PCM goes to the output buffers at SRAM 0x28100. The driver rings DOORBELL whenever the DSP may fill
 * the next buffer: after starting the stream, after handling a full buffer and after userland returns one (it then
 * clears the buffer's state). The DSP fills its buffers in turn, sets their sample count and state to 1, and raises
 * AMC_IRQ_OUTPUT. The handler treats AMC_IRQ_OUTPUT for a buffer that is not full as an error, so the model fills one
 * buffer per doorbell.
 *
 * The driver loads one codec image per stream; the model tells them apart by the memory windows the driver sets up
 * for it. It decodes AAC with faad2 (the sample rate comes from the host control block, the channel count from the
 * first syntax element), MP3 with mpg123, when QEMU is built with them, and ALAC with its own decoder. The output has
 * the stream's channels: userland plays a mono stream's output as mono. ALAC frames hold 4096 samples per channel, so
 * the model publishes a single output buffer for it.
 */
#include "hw/arm/ipod_touch_amc.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "exec/address-spaces.h"
#include "qemu/log.h"
#include "trace.h"
#ifdef CONFIG_FAAD2
#include <neaacdec.h>
#endif
#ifdef CONFIG_MPG123
#include <mpg123.h>
#endif

#define AAC_ID_SCE 0 // single channel element: the stream is mono

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

static uint16_t ipod_touch_amc_sram_read16(IPodTouchAMCState *s, hwaddr offset)
{
    uint8_t buf[2];
    address_space_read(s->sram_as, s->sram_base + offset, MEMTXATTRS_UNSPECIFIED, buf, sizeof(buf));
    return buf[0] | buf[1] << 8;
}

static void ipod_touch_amc_boot_done(void *opaque)
{
    IPodTouchAMCState *s = opaque;

    trace_ipod_touch_amc_boot_done();
    ipod_touch_amc_raise(s, AMC_IRQ_OUTPUT);
}

static const struct {
    uint32_t window;
    AMCCodec codec;
} amc_codec_windows[] = {
    { 0xc6005c00, AMC_CODEC_MP3 },
    { 0xc600b800, AMC_CODEC_AAC },
    { 0xc6008800, AMC_CODEC_ALAC },
    { 0xc6005600, AMC_CODEC_SPEECH },
};

static void ipod_touch_amc_decoder_close(IPodTouchAMCState *s)
{
    if (s->decoder_open) {
        switch (s->codec) {
#ifdef CONFIG_FAAD2
        case AMC_CODEC_AAC:
            NeAACDecClose(s->decoder);
            break;
#endif
#ifdef CONFIG_MPG123
        case AMC_CODEC_MP3:
            mpg123_delete(s->decoder);
            break;
#endif
        default:
            break;
        }
    }
    s->decoder = NULL;
    s->decoder_open = false;
    s->decoder_failed = false;
}

/* The DSP reboots: drop the stream. */
static void ipod_touch_amc_stream_reset(IPodTouchAMCState *s)
{
    g_byte_array_set_size(s->input, 0);
    s->in_received = s->in_consumed = 0;
    s->fill_requested = false;
    s->out_buf = 0;
    timer_del(s->dma_timer);
    timer_del(s->fill_timer);
    ipod_touch_amc_decoder_close(s);
}

/* The codec firmware is running: publish its output buffers. */
static void ipod_touch_amc_dsp_run(IPodTouchAMCState *s)
{
    uint32_t window = s->regs[AMC_MEM_WINDOW_CODEC / 4];

    s->codec = AMC_CODEC_UNKNOWN;
    for (int i = 0; i < ARRAY_SIZE(amc_codec_windows); i++) {
        if (amc_codec_windows[i].window == window) {
            s->codec = amc_codec_windows[i].codec;
        }
    }
    if (s->codec == AMC_CODEC_ALAC) {
        s->out_buffers = 1;
        s->out_samples = AMC_OUT_SAMPLES_ALAC;
    } else {
        s->out_buffers = 2;
        s->out_samples = AMC_OUT_SAMPLES;
    }
    assert(s->out_buffers * s->out_samples * 2 <= AMC_OUT_SPACE);
    trace_ipod_touch_amc_dsp_run(s->codec, s->out_buffers, s->out_samples);
    ipod_touch_amc_sram_write16(s, AMC_SRAM_OUT_DESC + AMC_OUT_NBUF, s->out_buffers);
    ipod_touch_amc_sram_write16(s, AMC_SRAM_OUT_DESC + AMC_OUT_LEN, s->out_samples);
}

static void ipod_touch_amc_dma_done(void *opaque)
{
    IPodTouchAMCState *s = opaque;

    s->ch_status[4] = 0;
    if (s->dma_irq) {
        ipod_touch_amc_raise(s, AMC_IRQ_CH4_DONE);
    }
}

static void ipod_touch_amc_consume(IPodTouchAMCState *s, size_t len)
{
    len = MIN(len, s->input->len);
    g_byte_array_remove_range(s->input, 0, len);
    s->in_consumed += len;
}

static void ipod_touch_amc_schedule_fill(IPodTouchAMCState *s)
{
    if (s->fill_requested && !timer_pending(s->fill_timer)) {
        timer_mod(s->fill_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + AMC_FILL_DELAY_NS);
    }
}

#ifdef CONFIG_FAAD2
static bool ipod_touch_amc_aac_open(IPodTouchAMCState *s)
{
    uint16_t rate = ipod_touch_amc_sram_read16(s, AMC_SRAM_CONTROL + AMC_CONTROL_RATE);
    unsigned long out_rate;
    unsigned char out_channels;

    if (rate > 11) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid AAC sample rate code %u\n", __func__, rate);
        return false;
    }

    // AudioSpecificConfig: AAC LC, the frequency index, the channel configuration
    uint8_t channels = (s->input->data[0] >> 5) == AAC_ID_SCE ? 1 : 2;
    uint8_t asc[2] = { (2 << 3) | (rate >> 1), ((rate & 1) << 7) | (channels << 3) };

    NeAACDecHandle h = NeAACDecOpen();
    NeAACDecConfigurationPtr config = NeAACDecGetCurrentConfiguration(h);
    config->outputFormat = FAAD_FMT_16BIT;
    config->downMatrix = 1;
    config->dontUpSampleImplicitSBR = 1;
    NeAACDecSetConfiguration(h, config);
    if (NeAACDecInit2(h, asc, sizeof(asc), &out_rate, &out_channels) < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: faad2 rejected the stream configuration\n", __func__);
        NeAACDecClose(h);
        return false;
    }
    trace_ipod_touch_amc_decoder_open(out_rate, out_channels);
    s->decoder = h;
    return true;
}

static unsigned ipod_touch_amc_aac_decode(IPodTouchAMCState *s, int16_t *out, unsigned max_samples)
{
    while (s->input->len) {
        NeAACDecFrameInfo info;
        int16_t *pcm = NeAACDecDecode(s->decoder, &info, s->input->data, s->input->len);
        unsigned long used = info.bytesconsumed;

        if (info.error) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s\n", __func__, NeAACDecGetErrorMessage(info.error));
            if (!used) {
                // the decoder lost track of the frame boundaries
                used = s->input->len;
            }
        }
        ipod_touch_amc_consume(s, used);
        if (info.error || !pcm || !info.samples || !info.channels) {
            continue;
        }

        unsigned samples = MIN(info.samples, max_samples);
        memcpy(out, pcm, samples * sizeof(*out));
        return samples;
    }
    return 0;
}
#endif

#ifdef CONFIG_MPG123
static bool ipod_touch_amc_mp3_open(IPodTouchAMCState *s)
{
    const long *rates;
    size_t nrates;
    int err;
    mpg123_handle *h = mpg123_new(NULL, &err);

    if (!h) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s\n", __func__, mpg123_plain_strerror(err));
        return false;
    }
    // 16-bit samples with the stream's rate and channels
    mpg123_format_none(h);
    mpg123_rates(&rates, &nrates);
    for (size_t i = 0; i < nrates; i++) {
        mpg123_format(h, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);
    }
    if (mpg123_open_feed(h) != MPG123_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s\n", __func__, mpg123_strerror(h));
        mpg123_delete(h);
        return false;
    }
    trace_ipod_touch_amc_decoder_open(0, 0);
    s->decoder = h;
    return true;
}

static unsigned ipod_touch_amc_mp3_decode(IPodTouchAMCState *s, int16_t *out, unsigned max_samples)
{
    mpg123_handle *h = s->decoder;

    // mpg123 finds the frames in the stream itself and buffers what it has not decoded yet
    if (s->input->len) {
        mpg123_feed(h, s->input->data, s->input->len);
        ipod_touch_amc_consume(s, s->input->len);
    }
    for (;;) {
        off_t num;
        unsigned char *audio;
        size_t bytes;
        int ret = mpg123_decode_frame(h, &num, &audio, &bytes);

        if (ret == MPG123_NEW_FORMAT) {
            long rate;
            int channels, encoding;
            mpg123_getformat(h, &rate, &channels, &encoding);
            trace_ipod_touch_amc_decoder_open(rate, channels);
            continue;
        }
        if (ret == MPG123_OK && bytes) {
            unsigned samples = MIN(bytes / 2, max_samples);
            memcpy(out, audio, samples * 2);
            return samples;
        }
        if (ret == MPG123_NEED_MORE) {
            return 0;
        }
        if (ret != MPG123_OK) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s\n", __func__, mpg123_strerror(h));
            return 0;
        }
    }
}
#endif

static bool ipod_touch_amc_alac_open(IPodTouchAMCState *s)
{
    uint16_t bit_depth = ipod_touch_amc_sram_read16(s, AMC_SRAM_CONTROL + AMC_CONTROL_ALAC_BIT_DEPTH);
    uint16_t pb = ipod_touch_amc_sram_read16(s, AMC_SRAM_CONTROL + AMC_CONTROL_ALAC_PB);
    uint16_t kb = ipod_touch_amc_sram_read16(s, AMC_SRAM_CONTROL + AMC_CONTROL_ALAC_KB);
    uint16_t mb = ipod_touch_amc_sram_read16(s, AMC_SRAM_CONTROL + AMC_CONTROL_ALAC_MB);

    trace_ipod_touch_amc_alac_params(bit_depth, pb, kb, mb);
    if (bit_depth != 16) {
        qemu_log_mask(LOG_UNIMP, "%s: %u-bit ALAC not supported\n", __func__, bit_depth);
        return false;
    }
    if (pb == 0 || pb > 255 || kb == 0 || kb > 31 || mb == 0 || mb > 255) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid ALAC parameters pb %u kb %u mb %u\n", __func__, pb, kb, mb);
        return false;
    }
    s->alac = (ALACConfig) {
        .frame_length = ALAC_MAX_FRAME_LENGTH, .bit_depth = bit_depth, .pb = pb, .mb = mb, .kb = kb,
    };
    return true;
}

static unsigned ipod_touch_amc_alac_decode(IPodTouchAMCState *s, int16_t *out, unsigned max_samples)
{
    size_t used;
    unsigned channels;
    int frames = alac_decode_frame(&s->alac, s->input->data, s->input->len, out, max_samples / ALAC_MAX_CHANNELS,
                                   &used, &channels);

    if (frames < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid ALAC frame, dropping the input\n", __func__);
        ipod_touch_amc_consume(s, s->input->len);
        return 0;
    }
    if (frames == 0) {
        return 0; // wait for the rest of the frame
    }
    ipod_touch_amc_consume(s, used);
    return frames * channels;
}

static bool ipod_touch_amc_decoder_start(IPodTouchAMCState *s)
{
    switch (s->codec) {
#ifdef CONFIG_FAAD2
    case AMC_CODEC_AAC:
        return ipod_touch_amc_aac_open(s);
#endif
#ifdef CONFIG_MPG123
    case AMC_CODEC_MP3:
        return ipod_touch_amc_mp3_open(s);
#endif
    case AMC_CODEC_ALAC:
        return ipod_touch_amc_alac_open(s);
    default:
        qemu_log_mask(LOG_UNIMP, "%s: no decoder for codec %u (QEMU built without its library?)\n", __func__,
                      s->codec);
        return false;
    }
}

/* Decodes the next frame that produces audio; returns the number of 16-bit samples written to out, 0 if it needs
 * more input. */
static unsigned ipod_touch_amc_decode(IPodTouchAMCState *s, int16_t *out, unsigned max_samples)
{
    switch (s->codec) {
#ifdef CONFIG_FAAD2
    case AMC_CODEC_AAC:
        return ipod_touch_amc_aac_decode(s, out, max_samples);
#endif
#ifdef CONFIG_MPG123
    case AMC_CODEC_MP3:
        return ipod_touch_amc_mp3_decode(s, out, max_samples);
#endif
    case AMC_CODEC_ALAC:
        return ipod_touch_amc_alac_decode(s, out, max_samples);
    default:
        return 0;
    }
}

/* Decodes the next output buffer, if the driver asked for one and the DSP has the input and a free buffer. */
static void ipod_touch_amc_fill(void *opaque)
{
    IPodTouchAMCState *s = opaque;
    unsigned buf = s->out_buf;

    // the driver is still handling the previous buffer, or returns this one later, which rings the doorbell again
    if (!s->fill_requested || (s->irq_status[0] & AMC_IRQ_OUTPUT) ||
        ipod_touch_amc_sram_read16(s, AMC_SRAM_OUT_DESC + AMC_OUT_STATE(buf)) != 0) {
        return;
    }
    if (!s->decoder_open) {
        // the decoders configure themselves from the first input
        if (!s->input->len || s->decoder_failed) {
            // without a decoder, the input is dropped so the driver does not stall
            ipod_touch_amc_consume(s, s->input->len);
            return;
        }
        s->decoder_open = ipod_touch_amc_decoder_start(s);
        s->decoder_failed = !s->decoder_open;
        if (!s->decoder_open) {
            ipod_touch_amc_consume(s, s->input->len);
            return;
        }
    }

    int16_t pcm[AMC_OUT_MAX_SAMPLES];
    unsigned samples = ipod_touch_amc_decode(s, pcm, s->out_samples);

    if (!samples) {
        return; // wait for more input
    }
    for (unsigned i = 0; i < samples; i++) {
        pcm[i] = cpu_to_le16(pcm[i]);
    }
    address_space_write(s->sram_as, s->sram_base + AMC_SRAM_OUT_PCM + buf * s->out_samples * 2,
                        MEMTXATTRS_UNSPECIFIED, pcm, samples * 2);
    ipod_touch_amc_sram_write16(s, AMC_SRAM_OUT_DESC + AMC_OUT_COUNT(buf), samples);
    ipod_touch_amc_sram_write16(s, AMC_SRAM_OUT_DESC + AMC_OUT_STATE(buf), 1);
    trace_ipod_touch_amc_output(buf, samples, s->in_consumed);
    s->out_buf = (buf + 1) % s->out_buffers;
    s->fill_requested = false;
    ipod_touch_amc_raise(s, AMC_IRQ_OUTPUT);
}

static void ipod_touch_amc_dump_input(IPodTouchAMCState *s, const uint8_t *data, size_t len)
{
    FILE *f = fopen(s->input_dump, "ab");
    if (!f) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: cannot open %s\n", __func__, s->input_dump);
        return;
    }
    fwrite(data, 1, len, f);
    fclose(f);
}

/* Channel 4 transfers compressed data from DRAM into the DSP's input FIFO: collect it. */
static void ipod_touch_amc_ch4_start(IPodTouchAMCState *s, uint32_t start)
{
    uint32_t next = start;
    int count = 0;

    if (s->ch_status[4] & 7) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: channel 4 started while not idle\n", __func__);
        return;
    }
    s->dma_irq = false;
    while (next) {
        uint32_t desc[4], len, src;

        if ((next & 3) != AMC_DESC_PHYS || ++count > AMC_DESC_MAX_CHAIN) {
            qemu_log_mask(LOG_UNIMP, "%s: unsupported descriptor 0x%08x\n", __func__, next);
            break;
        }
        address_space_read(s->sram_as, next & AMC_DESC_ADDR_MASK, MEMTXATTRS_UNSPECIFIED, desc, sizeof(desc));
        for (int i = 0; i < 4; i++) {
            desc[i] = le32_to_cpu(desc[i]);
        }
        len = desc[AMC_DESC_CTRL / 4] >> 16;
        src = desc[AMC_DESC_SRC / 4];
        trace_ipod_touch_amc_ch4_desc(next & AMC_DESC_ADDR_MASK, desc[AMC_DESC_CTRL / 4], src, desc[AMC_DESC_NEXT / 4]);

        size_t old = s->input->len;
        g_byte_array_set_size(s->input, old + len);
        address_space_read(s->sram_as, src, MEMTXATTRS_UNSPECIFIED, s->input->data + old, len);
        s->in_received += len;
        if (s->input_dump) {
            ipod_touch_amc_dump_input(s, s->input->data + old, len);
        }
        if (desc[AMC_DESC_CTRL / 4] & AMC_DESC_CTRL_IRQ) {
            s->dma_irq = true;
        }
        next = desc[AMC_DESC_NEXT / 4];
    }
    s->ch_status[4] = AMC_CH_STATUS_BUSY;
    timer_mod(s->dma_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + AMC_DMA_DELAY_NS);
    ipod_touch_amc_schedule_fill(s);
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
    case AMC_CH_CMD_DONE:
        // the driver cannot clear AMC_IRQ_CH4_DONE through IRQ0_ACK, which only covers the low 15 bits
        if (ch == 4) {
            s->irq_status[0] &= ~AMC_IRQ_CH4_DONE;
        }
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
        ipod_touch_amc_schedule_fill(s);
        break;
    case AMC_IRQ1_ENABLE:
        s->irq_enabled[1] |= val;
        break;
    case AMC_IRQ1_DISABLE:
        s->irq_enabled[1] &= ~val;
        break;
    case AMC_DSP_START:
        // a reboot drops the previous stream; the boot image signals that it runs
        ipod_touch_amc_stream_reset(s);
        timer_mod(s->boot_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + AMC_BOOT_DELAY_NS);
        break;
    case AMC_DSP_RUN:
        ipod_touch_amc_dsp_run(s);
        break;
    case AMC_DOORBELL:
        s->fill_requested = true;
        ipod_touch_amc_schedule_fill(s);
        break;
    default:
        ch = ipod_touch_amc_channel(addr, &reg);
        if (ch >= 0 && reg == AMC_CH_CTRL) {
            ipod_touch_amc_channel_ctrl(s, ch, val);
        } else if (ch == 4 && reg == AMC_CH_START) {
            ipod_touch_amc_ch4_start(s, val);
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
    ipod_touch_amc_stream_reset(s);
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
    s->dma_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ipod_touch_amc_dma_done, s);
    s->fill_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ipod_touch_amc_fill, s);
    s->input = g_byte_array_new();
    s->sram_as = &address_space_memory;
}

static void ipod_touch_amc_finalize(Object *obj)
{
    IPodTouchAMCState *s = IPOD_TOUCH_AMC(obj);
    timer_free(s->boot_timer);
    timer_free(s->dma_timer);
    timer_free(s->fill_timer);
    ipod_touch_amc_decoder_close(s);
    g_byte_array_free(s->input, true);
}

static Property ipod_touch_amc_properties[] = {
    DEFINE_PROP_UINT64("sram-base", IPodTouchAMCState, sram_base, 0x22000000),
    DEFINE_PROP_STRING("input-dump", IPodTouchAMCState, input_dump),
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
        VMSTATE_TIMER_PTR(dma_timer, IPodTouchAMCState),
        VMSTATE_BOOL(dma_irq, IPodTouchAMCState),
        VMSTATE_UINT32(codec, IPodTouchAMCState),
        VMSTATE_UINT32(out_buffers, IPodTouchAMCState),
        VMSTATE_UINT32(out_samples, IPodTouchAMCState),
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
