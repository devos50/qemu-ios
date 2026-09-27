#ifndef HW_ARM_IPOD_TOUCH_I2S_H
#define HW_ARM_IPOD_TOUCH_I2S_H

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "audio/audio.h"

#define TYPE_IPOD_TOUCH_I2S "ipodtouch.i2s"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchI2SState, IPOD_TOUCH_I2S)

#define I2S_CLKCON 0x00
#define I2S_TXCON  0x04
#define I2S_TXCOM  0x08
#define I2S_TXFIFO 0x10
#define I2S_RXCON  0x30
#define I2S_RXCOM  0x34
#define I2S_RXFIFO 0x38
#define I2S_STATUS 0x3C
#define I2S_CFG40  0x40 // written once during configuration, meaning unknown

#define I2S_CLKCON_ENABLE    (1 << 0)
#define I2S_TXCON_ENABLE     (1 << 0)
#define I2S_TXCON_SYNC_START (1 << 20) // the driver waits for a frame sync interrupt before starting
#define I2S_COM_START        0x6       // written to TXCOM/RXCOM to start, 0 stops

// Interval of the frame sync edges and of moving samples between the DMA and the audio output
#define I2S_TICK_NS     (1 * SCALE_MS)
// Samples due after a longer stall (e.g. the VM was paused) are dropped instead of transferred at once
#define I2S_MAX_CATCHUP (50 * SCALE_MS)
// Samples buffered between the virtual-time DMA and the host audio output
#define I2S_OUT_FRAMES  8192

typedef struct IPodTouchI2SState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    QEMUTimer *timer;
    qemu_irq sync_irq; // frame sync edge, to the GPIO interrupt controller
    qemu_irq tx_dreq;  // DMA request lines
    qemu_irq rx_dreq;

    uint32_t clkcon;
    uint32_t txcon;
    uint32_t txcom;
    uint32_t rxcon;
    uint32_t rxcom;
    uint32_t status;
    uint32_t cfg40;

    uint32_t sample_rate;
    int64_t tx_start_ns;   // virtual time at which the transmitter started
    uint64_t tx_frames;    // frames due since then
    uint32_t tx_room;      // samples the TX FIFO accepts before it deasserts its DMA request
    uint32_t tx_sample;    // samples written to the TX FIFO in the current frame
    int64_t rx_start_ns;
    uint64_t rx_frames;
    uint32_t rx_avail;     // samples the RX FIFO provides before it deasserts its DMA request

    // Host audio output: a ring of stereo frames filled at the sample rate in virtual time
    QEMUSoundCard card;
    SWVoiceOut *voice;
    int16_t out[I2S_OUT_FRAMES][2];
    uint32_t out_head;     // next frame to write
    uint32_t out_fill;
    int16_t frame[2];      // frame being assembled from the TX FIFO
} IPodTouchI2SState;

void ipod_touch_i2s_set_sample_rate(IPodTouchI2SState *s, uint32_t rate);

#endif
