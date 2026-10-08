#ifndef HW_ARM_IPOD_TOUCH_AMC_H
#define HW_ARM_IPOD_TOUCH_AMC_H

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/arm/ipod_touch_alac.h"

#define TYPE_IPOD_TOUCH_AMC "ipodtouch.amc"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchAMCState, IPOD_TOUCH_AMC)

#define AMC_REG_SIZE  0x3000
#define AMC_SRAM_SIZE 0x30000

// DMA channels: 0-3 at n * 0x20, 4-7 at these offsets
#define AMC_NUM_CHANNELS 8
#define AMC_CH_START     0x00 // descriptor physical address | 1 or 2
#define AMC_CH_CTRL      0x10 // command
#define AMC_CH_STATUS    0x14 // bits 2:0: 0 idle, 7 halted

#define AMC_CH_CMD_HALT   0x04
#define AMC_CH_CMD_RESUME 0x10
#define AMC_CH_CMD_DONE   0x20 // acknowledges a finished transfer
#define AMC_CH_CMD_RESET  0x60
#define AMC_CH_STATUS_BUSY   1
#define AMC_CH_STATUS_HALTED 7

// Descriptor and START/next encoding: address | 1 for a physical address, | 2 for a DSP address
#define AMC_DESC_PHYS     1
#define AMC_DESC_DSP      2
#define AMC_DESC_ADDR_MASK (~3u)
#define AMC_DESC_NEXT     0x00
#define AMC_DESC_CTRL     0x04 // bits 31:16 length in bytes, bit 4 interrupt when done, bits 11:7 burst
#define AMC_DESC_SRC      0x08
#define AMC_DESC_DST      0x0c
#define AMC_DESC_CTRL_IRQ (1 << 4)
#define AMC_DESC_MAX_CHAIN 256

// Time a channel 4 transfer takes before it reports completion
#define AMC_DMA_DELAY_NS  (50 * SCALE_US)
// Time the DSP takes to fill an output buffer after the driver asked for one
#define AMC_FILL_DELAY_NS (100 * SCALE_US)

// Read FIFOs of channels 4 and 5, drained by the driver after halting them
#define AMC_CH4_FIFO_DATA  0x15c
#define AMC_CH4_FIFO_COUNT 0x160
#define AMC_CH5_FIFO_DATA  0x1dc
#define AMC_CH5_FIFO_COUNT 0x1e0

// Mode bit of channels 4 and 5, written before starting a transfer
#define AMC_CH4_MODE       0x118
#define AMC_CH5_MODE       0x198

#define AMC_MEM_WINDOWS    0x938 // 18 memory window descriptors, up to 0x97c
#define AMC_MEM_WINDOW_CODEC 0x960 // window 10, which differs between the codec images (table at 0xc02c07c8)
#define AMC_DSP_START      0x984 // start the DSP (written 0 after loading the boot image)
#define AMC_DSP_RUN        0x988 // written 0x100 after loading the codec image
#define AMC_DOORBELL       0x98c // written 1 when the driver frees an output buffer
#define AMC_DSP_CONFIG     0x99c // written 3 before loading the firmware
#define AMC_CLOCK_DIV      0x1000 // DSP clock divider - 1

// Interrupts, two banks: writing 1 bits to ENABLE unmasks them, to DISABLE masks them; STATUS is the raw state
#define AMC_IRQ0_ENABLE  0xa8c
#define AMC_IRQ0_DISABLE 0xa90
#define AMC_IRQ0_STATUS  0xa98
#define AMC_IRQ0_ACK     0xc48
#define AMC_IRQ1_ENABLE  0xb0c
#define AMC_IRQ1_DISABLE 0xb10
#define AMC_IRQ1_STATUS  0xb18
#define AMC_IRQ0_MASK    0x7ffffff
#define AMC_IRQ0_ACK_MASK 0x7fff

#define AMC_IRQ_OUTPUT   (1 << 2)  // DSP booted / an output buffer is ready
#define AMC_IRQ_STATUS   (1 << 12) // status block in SRAM updated
#define AMC_IRQ_CH4_DONE (1 << 18) // input DMA finished

// Shared SRAM structures (the layout without the "r2h2" flag)
#define AMC_SRAM_OUT_DESC  0x28000 // output buffer descriptor, u16 fields
#define AMC_SRAM_OUT_PCM   0x28100 // output buffers, back to back
#define AMC_SRAM_CONTROL   0x2ff00 // host control block
#define AMC_SRAM_STATUS    0x2ff28 // DSP status block

// Output buffer descriptor fields (byte offsets of u16 values)
#define AMC_OUT_NBUF       0x2 // number of output buffers, 1 or 2
#define AMC_OUT_LEN        0x4 // capacity of each buffer in 16-bit samples
#define AMC_OUT_STATE(i)   (0xa + (i) * 4) // 1 when the DSP filled the buffer, the host clears it on returning it
#define AMC_OUT_COUNT(i)   (0xc + (i) * 4) // 16-bit samples in the buffer

// Host control block fields (u16), written by the driver with the first input (c02aff8c): flags (OR-ed into the
// image's defaults) and, for AAC, the sample rate code (the MPEG-4 frequency index)
#define AMC_CONTROL_FLAGS  0x0
#define AMC_CONTROL_RATE   0x6
#define AMC_CONTROL_AAC    0x6 // flag bits the driver sets for AAC
#define AMC_CONTROL_ALAC   0x8 // only set for ALAC (0x1e), which shares the AAC bits

// Output buffers the HLE reports: two buffers that hold one MP3 frame (1152 stereo frames), the larger of AAC/MP3,
// or for ALAC a single buffer for one frame (4096 stereo frames), as two do not fit below the control block
#define AMC_OUT_SPACE      (AMC_SRAM_CONTROL - AMC_SRAM_OUT_PCM)
#define AMC_OUT_SAMPLES    (1152 * 2)
#define AMC_OUT_SAMPLES_ALAC (ALAC_MAX_FRAME_LENGTH * ALAC_MAX_CHANNELS)
#define AMC_OUT_MAX_SAMPLES AMC_OUT_SAMPLES_ALAC

// ALAC parameters in the host control block (u16 fields with bytes of the stream's ALACSpecificConfig)
#define AMC_CONTROL_ALAC_BIT_DEPTH 0x4
#define AMC_CONTROL_ALAC_PB        0x6
#define AMC_CONTROL_ALAC_KB        0x8
#define AMC_CONTROL_ALAC_MB        0xa

typedef enum AMCCodec {
    AMC_CODEC_UNKNOWN,
    AMC_CODEC_MP3,
    AMC_CODEC_AAC,
    AMC_CODEC_ALAC,
    AMC_CODEC_SPEECH,
} AMCCodec;

// Delay between the driver starting the DSP and the "booted" interrupt
#define AMC_BOOT_DELAY_NS  (10 * SCALE_US)

typedef struct IPodTouchAMCState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *boot_timer;
    QEMUTimer *dma_timer;
    QEMUTimer *fill_timer;
    AddressSpace *sram_as;
    uint64_t sram_base;

    uint32_t regs[AMC_REG_SIZE / 4]; // last written value of registers without side effects
    uint32_t ch_ctrl[AMC_NUM_CHANNELS];
    uint32_t ch_status[AMC_NUM_CHANNELS];
    uint32_t irq_status[2];
    uint32_t irq_enabled[2];

    // Compressed data the driver sent through channel 4 and the decoder has not consumed yet. Positions count bytes
    // since the DSP last booted.
    GByteArray *input;
    uint64_t in_received;
    uint64_t in_consumed;
    bool dma_irq;        // the running channel 4 transfer interrupts when done
    char *input_dump;    // debugging: append everything channel 4 transfers to this file

    // Output: the driver rings the doorbell to ask for the next buffer, which the DSP fills in turn
    uint32_t codec;       // AMCCodec of the image the driver loaded
    uint32_t out_buffers;
    uint32_t out_samples; // capacity of each output buffer
    bool fill_requested;
    uint32_t out_buf;
    bool decoder_open;
    bool decoder_failed;
    void *decoder;        // faad2 or mpg123 handle
    ALACConfig alac;
} IPodTouchAMCState;

#endif
