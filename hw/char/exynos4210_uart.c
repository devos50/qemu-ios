/*
 *  Exynos4210 UART Emulation
 *
 *  Copyright (C) 2011 Samsung Electronics Co Ltd.
 *    Maksim Kozlov, <m.kozlov@samsung.com>
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "chardev/char-fe.h"
#include "chardev/char-serial.h"

#include "hw/arm/exynos4210.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"

#include "trace.h"
#include "qom/object.h"

/*
 *  Offsets for UART registers relative to SFR base address
 *  for UARTn
 *
 */
#define ULCON      0x0000 /* Line Control             */
#define UCON       0x0004 /* Control                  */
#define UFCON      0x0008 /* FIFO Control             */
#define UMCON      0x000C /* Modem Control            */
#define UTRSTAT    0x0010 /* Tx/Rx Status             */
#define UERSTAT    0x0014 /* UART Error Status        */
#define UFSTAT     0x0018 /* FIFO Status              */
#define UMSTAT     0x001C /* Modem Status             */
#define UTXH       0x0020 /* Transmit Buffer          */
#define URXH       0x0024 /* Receive Buffer           */
#define UBRDIV     0x0028 /* Baud Rate Divisor        */
#define UFRACVAL   0x002C /* Divisor Fractional Value */
#define UINTP      0x0030 /* Interrupt Pending        */
#define UINTSP     0x0034 /* Interrupt Source Pending */
#define UINTM      0x0038 /* Interrupt Mask           */

/*
 * for indexing register in the uint32_t array
 *
 * 'reg' - register offset (see offsets definitions above)
 *
 */
#define I_(reg) (reg / sizeof(uint32_t))

typedef struct Exynos4210UartReg {
    const char         *name; /* the only reason is the debug output */
    hwaddr  offset;
    uint32_t            reset_value;
} Exynos4210UartReg;

static const Exynos4210UartReg exynos4210_uart_regs[] = {
    {"ULCON",    ULCON,    0x00000000},
    {"UCON",     UCON,     0x00003000},
    {"UFCON",    UFCON,    0x00000000},
    {"UMCON",    UMCON,    0x00000000},
    {"UTRSTAT",  UTRSTAT,  0x00000006}, /* RO */
    {"UERSTAT",  UERSTAT,  0x00000000}, /* RO */
    {"UFSTAT",   UFSTAT,   0x00000000}, /* RO */
    {"UMSTAT",   UMSTAT,   0x00000000}, /* RO */
    {"UTXH",     UTXH,     0x5c5c5c5c}, /* WO, undefined reset value*/
    {"URXH",     URXH,     0x00000000}, /* RO */
    {"UBRDIV",   UBRDIV,   0x00000000},
    {"UFRACVAL", UFRACVAL, 0x00000000},
    {"UINTP",    UINTP,    0x00000000},
    {"UINTSP",   UINTSP,   0x00000000},
    {"UINTM",    UINTM,    0x00000000},
};

#define EXYNOS4210_UART_REGS_MEM_SIZE    0x3C

/* UART FIFO Control */
#define UFCON_FIFO_ENABLE                    0x1
#define UFCON_Rx_FIFO_RESET                  0x2
#define UFCON_Tx_FIFO_RESET                  0x4
#define UFCON_Tx_FIFO_TRIGGER_LEVEL_SHIFT    8
#define UFCON_Tx_FIFO_TRIGGER_LEVEL (7 << UFCON_Tx_FIFO_TRIGGER_LEVEL_SHIFT)
#define UFCON_Rx_FIFO_TRIGGER_LEVEL_SHIFT    4
#define UFCON_Rx_FIFO_TRIGGER_LEVEL (7 << UFCON_Rx_FIFO_TRIGGER_LEVEL_SHIFT)

/* Uart FIFO Status */
#define UFSTAT_Rx_FIFO_COUNT        0xff
#define UFSTAT_Rx_FIFO_FULL         0x100
#define UFSTAT_Rx_FIFO_ERROR        0x200
#define UFSTAT_Tx_FIFO_COUNT_SHIFT  16
#define UFSTAT_Tx_FIFO_COUNT        (0xff << UFSTAT_Tx_FIFO_COUNT_SHIFT)
#define UFSTAT_Tx_FIFO_FULL_SHIFT   24
#define UFSTAT_Tx_FIFO_FULL         (1 << UFSTAT_Tx_FIFO_FULL_SHIFT)

/* UART Interrupt Source Pending */
#define UINTSP_RXD      0x1 /* Receive interrupt  */
#define UINTSP_ERROR    0x2 /* Error interrupt    */
#define UINTSP_TXD      0x4 /* Transmit interrupt */
#define UINTSP_MODEM    0x8 /* Modem interrupt    */

/* UART Line Control */
#define ULCON_IR_MODE_SHIFT   6
#define ULCON_PARITY_SHIFT    3
#define ULCON_STOP_BIT_SHIFT  1

/* UART Tx/Rx Status */
#define UTRSTAT_Rx_TIMEOUT              0x8
#define UTRSTAT_TRANSMITTER_EMPTY       0x4
#define UTRSTAT_Tx_BUFFER_EMPTY         0x2
#define UTRSTAT_Rx_BUFFER_DATA_READY    0x1

/*
 * S5L8720 acknowledges interrupts through UTRSTAT (write-one-to-clear),
 * with enables in UCON[14:11]. The 7E18 ISR at 0xc059aecc handles timeout
 * (0x08), receive threshold (0x10), transmit (0x20), and error (0x40).
 * Bit 0x100 is AUTOBAUD, not receive: its handler divides the clock by
 * UABRCNT at offset 0x2c. Mapping RX there hid the receive-timeout callback
 * and made the Bluetooth client block SpringBoard's launch animation.
 */
#define UTRSTAT_S5L_Tx_INT              0x20
#define UTRSTAT_S5L_Rx_INT              0x10
#define UTRSTAT_S5L_Err_INT             0x40
#define UCON_S5L_TIMEOUT_INT_ENABLE     (1 << 11)
#define UCON_S5L_RX_INT_ENABLE          (1 << 12)
#define UCON_S5L_TX_INT_ENABLE          (1 << 13)
#define UCON_S5L_ERROR_INT_ENABLE       (1 << 14)

/* UART Error Status */
#define UERSTAT_OVERRUN  0x1
#define UERSTAT_PARITY   0x2
#define UERSTAT_FRAME    0x4
#define UERSTAT_BREAK    0x8

typedef struct {
    uint8_t    *data;
    uint32_t    sp, rp; /* store and retrieve pointers */
    uint32_t    size;
} Exynos4210UartFIFO;

#define TYPE_EXYNOS4210_UART "exynos4210.uart"
OBJECT_DECLARE_SIMPLE_TYPE(Exynos4210UartState, EXYNOS4210_UART)

struct Exynos4210UartState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    uint32_t             reg[EXYNOS4210_UART_REGS_MEM_SIZE / sizeof(uint32_t)];
    Exynos4210UartFIFO   rx;
    Exynos4210UartFIFO   tx;

    QEMUTimer *fifo_timeout_timer;
    uint64_t wordtime;        /* word time in ns */

    CharBackend       chr;
    qemu_irq          irq;
    qemu_irq          dmairq;
    /*
     * Rx DMA REQUEST, as the peripheral actually drives it: high only while
     * the guest has Rx DMA selected AND there are bytes to fetch. dmairq
     * cannot be reused for this -- it also reads low when Rx DMA is disabled,
     * and asserting a request then makes the DMAC read an empty FIFO, which is
     * the invented-data wedge described at pl080_attach_paced_peripheral() in
     * hw/arm/ipod_touch_2g.c.
     */
    qemu_irq          rxdmareq;
    /* Retain receive activity across a DMA-mode change until its timeout. */
    bool              rx_since_timeout;

    uint32_t channel;
    bool s5l8720_irq;

};


/* Used only for tracing */
static const char *exynos4210_uart_regname(hwaddr  offset)
{

    int i;

    for (i = 0; i < ARRAY_SIZE(exynos4210_uart_regs); i++) {
        if (offset == exynos4210_uart_regs[i].offset) {
            return exynos4210_uart_regs[i].name;
        }
    }

    return NULL;
}


static void fifo_store(Exynos4210UartFIFO *q, uint8_t ch)
{
    q->data[q->sp] = ch;
    q->sp = (q->sp + 1) % q->size;
}

static uint8_t fifo_retrieve(Exynos4210UartFIFO *q)
{
    uint8_t ret = q->data[q->rp];
    q->rp = (q->rp + 1) % q->size;
    return  ret;
}

static int fifo_elements_number(const Exynos4210UartFIFO *q)
{
    if (q->sp < q->rp) {
        return q->size - q->rp + q->sp;
    }

    return q->sp - q->rp;
}

static int fifo_empty_elements_number(const Exynos4210UartFIFO *q)
{
    return q->size - fifo_elements_number(q);
}

static void fifo_reset(Exynos4210UartFIFO *q)
{
    g_free(q->data);
    q->data = NULL;

    q->data = g_malloc0(q->size);

    q->sp = 0;
    q->rp = 0;
}

static uint32_t exynos4210_uart_FIFO_trigger_level(uint32_t channel,
                                                   uint32_t reg)
{
    uint32_t level;

    switch (channel) {
    case 0:
        level = reg * 32;
        break;
    case 1:
    case 4:
        level = reg * 8;
        break;
    case 2:
    case 3:
        level = reg * 2;
        break;
    default:
        level = 0;
        trace_exynos_uart_channel_error(channel);
        break;
    }
    return level;
}

static uint32_t
exynos4210_uart_Tx_FIFO_trigger_level(const Exynos4210UartState *s)
{
    uint32_t reg;

    reg = (s->reg[I_(UFCON)] & UFCON_Tx_FIFO_TRIGGER_LEVEL) >>
            UFCON_Tx_FIFO_TRIGGER_LEVEL_SHIFT;

    return exynos4210_uart_FIFO_trigger_level(s->channel, reg);
}

static uint32_t
exynos4210_uart_Rx_FIFO_trigger_level(const Exynos4210UartState *s)
{
    uint32_t reg;

    reg = ((s->reg[I_(UFCON)] & UFCON_Rx_FIFO_TRIGGER_LEVEL) >>
            UFCON_Rx_FIFO_TRIGGER_LEVEL_SHIFT) + 1;

    return exynos4210_uart_FIFO_trigger_level(s->channel, reg);
}

/*
 * Update Rx DMA busy signal if Rx DMA is enabled. For simplicity,
 * mark DMA as busy if DMA is enabled and the receive buffer is empty.
 */
static void exynos4210_uart_update_dmabusy(Exynos4210UartState *s)
{
    /*
     * UCON[1:0] selects the receive mode. The exynos TRM defines 10b as DMA
     * and leaves 11b reserved, but the S5L8720 in the iPod touch uses 11b --
     * its Bluetooth driver writes UCON=0x10040f, arms a peripheral->memory
     * channel on URXH, and then never reads the register itself. Reading 11b
     * as "not DMA" left the HCI reply sitting in the Rx FIFO: the driver saw
     * UFSTAT report the bytes, saw no DMA progress, reset the FIFO and started
     * over, so BTServer retried HCI_Reset forever. Accepting both encodings is
     * safe for a real exynos guest, which never programs the reserved one.
     */
    bool rx_dma_enabled = (s->reg[I_(UCON)] & 0x03) >= 0x02;
    uint32_t count = fifo_elements_number(&s->rx);

    if (rx_dma_enabled && !count) {
        qemu_irq_raise(s->dmairq);
        trace_exynos_uart_dmabusy(s->channel);
    } else {
        qemu_irq_lower(s->dmairq);
        trace_exynos_uart_dmaready(s->channel);
    }

    qemu_set_irq(s->rxdmareq, rx_dma_enabled && count > 0);
}

static void exynos4210_uart_update_irq(Exynos4210UartState *s)
{
    /*
     * The Tx interrupt is always requested if the number of data in the
     * transmit FIFO is smaller than the trigger level.
     */
    if (s->reg[I_(UFCON)] & UFCON_FIFO_ENABLE) {
        uint32_t count = (s->reg[I_(UFSTAT)] & UFSTAT_Tx_FIFO_COUNT) >>
                UFSTAT_Tx_FIFO_COUNT_SHIFT;

        if (count <= exynos4210_uart_Tx_FIFO_trigger_level(s) &&
            !s->s5l8720_irq) {
            /* S5L latches transmit completion on UTXH writes below. */
            s->reg[I_(UINTSP)] |= UINTSP_TXD;
        }

        /*
         * Rx interrupt if trigger level is reached or if rx timeout
         * interrupt is disabled and there is data in the receive buffer
         */
        count = fifo_elements_number(&s->rx);
        if ((count && !(s->reg[I_(UCON)] & 0x80)) ||
            count >= exynos4210_uart_Rx_FIFO_trigger_level(s)) {
            exynos4210_uart_update_dmabusy(s);
            s->reg[I_(UINTSP)] |= UINTSP_RXD;
            timer_del(s->fifo_timeout_timer);
        }
    } else if (s->reg[I_(UTRSTAT)] & UTRSTAT_Rx_BUFFER_DATA_READY) {
        exynos4210_uart_update_dmabusy(s);
        s->reg[I_(UINTSP)] |= UINTSP_RXD;
    }

    s->reg[I_(UINTP)] = s->reg[I_(UINTSP)] & ~s->reg[I_(UINTM)];

    /* S5L pending bits remain readable while their UCON enable is clear. */
    if (s->s5l8720_irq) {
        if (s->reg[I_(UINTP)] & UINTSP_TXD) {
            s->reg[I_(UTRSTAT)] |= UTRSTAT_S5L_Tx_INT;
        } else {
            s->reg[I_(UTRSTAT)] &= ~UTRSTAT_S5L_Tx_INT;
        }
        if (s->reg[I_(UINTP)] & UINTSP_RXD) {
            s->reg[I_(UTRSTAT)] |= UTRSTAT_S5L_Rx_INT;
        } else {
            s->reg[I_(UTRSTAT)] &= ~UTRSTAT_S5L_Rx_INT;
        }
        if (s->reg[I_(UINTP)] & UINTSP_ERROR) {
            s->reg[I_(UTRSTAT)] |= UTRSTAT_S5L_Err_INT;
        } else {
            s->reg[I_(UTRSTAT)] &= ~UTRSTAT_S5L_Err_INT;
        }
    }

    uint32_t pending = s->reg[I_(UINTP)];
    if (s->s5l8720_irq) {
        uint32_t enabled = s->reg[I_(UCON)];
        if (!(enabled & UCON_S5L_TX_INT_ENABLE)) {
            pending &= ~UINTSP_TXD;
        }
        if (!(enabled & UCON_S5L_RX_INT_ENABLE)) {
            pending &= ~UINTSP_RXD;
        }
        if (!(enabled & UCON_S5L_ERROR_INT_ENABLE)) {
            pending &= ~UINTSP_ERROR;
        }
        if ((enabled & UCON_S5L_TIMEOUT_INT_ENABLE) &&
            (s->reg[I_(UTRSTAT)] & UTRSTAT_Rx_TIMEOUT)) {
            pending |= UINTSP_RXD;
        }
    }
    if (pending) {
        qemu_irq_raise(s->irq);
        trace_exynos_uart_irq_raised(s->channel, s->reg[I_(UINTP)]);
    } else {
        qemu_irq_lower(s->irq);
        trace_exynos_uart_irq_lowered(s->channel);
    }
}

static void exynos4210_uart_timeout_int(void *opaque)
{
    Exynos4210UartState *s = opaque;

    trace_exynos_uart_rx_timeout(s->channel, s->reg[I_(UTRSTAT)],
                                 s->reg[I_(UINTSP)]);

    if ((s->reg[I_(UTRSTAT)] & UTRSTAT_Rx_BUFFER_DATA_READY) ||
        (s->reg[I_(UCON)] & (1 << 11))) {
        s->reg[I_(UINTSP)] |= UINTSP_RXD;
        s->reg[I_(UTRSTAT)] |= UTRSTAT_Rx_TIMEOUT;
        exynos4210_uart_update_dmabusy(s);
        exynos4210_uart_update_irq(s);
        /*
         * The UART timeout wakes the driver's partial-buffer path. Do not
         * synthesize a DMAC terminal count: UART1 uses controller flow with
         * a 2048-byte LLI, and advancing it after a short HCI reply makes the
         * guest consume the unfilled tail as Bluetooth packet data.
         */
        s->rx_since_timeout = false;
    }
}

static void exynos4210_uart_update_parameters(Exynos4210UartState *s)
{
    int speed, parity, data_bits, stop_bits;
    QEMUSerialSetParams ssp;
    uint64_t uclk_rate;

    if (s->reg[I_(UBRDIV)] == 0) {
        return;
    }

    if (s->reg[I_(ULCON)] & 0x20) {
        if (s->reg[I_(ULCON)] & 0x28) {
            parity = 'E';
        } else {
            parity = 'O';
        }
    } else {
        parity = 'N';
    }

    if (s->reg[I_(ULCON)] & 0x4) {
        stop_bits = 2;
    } else {
        stop_bits = 1;
    }

    data_bits = (s->reg[I_(ULCON)] & 0x3) + 5;

    uclk_rate = 24000000;

    speed = uclk_rate / ((16 * (s->reg[I_(UBRDIV)]) & 0xffff) +
            (s->reg[I_(UFRACVAL)] & 0x7) + 16);

    ssp.speed     = speed;
    ssp.parity    = parity;
    ssp.data_bits = data_bits;
    ssp.stop_bits = stop_bits;

    s->wordtime = NANOSECONDS_PER_SECOND * (data_bits + stop_bits + 1) / speed;

    qemu_chr_fe_ioctl(&s->chr, CHR_IOCTL_SERIAL_SET_PARAMS, &ssp);

    trace_exynos_uart_update_params(
                s->channel, speed, parity, data_bits, stop_bits, s->wordtime);
}

static void exynos4210_uart_rx_timeout_set(Exynos4210UartState *s)
{
    if ((s->reg[I_(UCON)] & 3) && (s->reg[I_(UCON)] & 0x80)) {
        uint32_t timeout = ((s->reg[I_(UCON)] >> 12) & 0x0f) * s->wordtime;

        timer_mod(s->fifo_timeout_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + timeout);
    } else {
        timer_del(s->fifo_timeout_timer);
    }
}

static void exynos4210_uart_write(void *opaque, hwaddr offset,
                               uint64_t val, unsigned size)
{
    Exynos4210UartState *s = (Exynos4210UartState *)opaque;
    uint8_t ch;

    trace_exynos_uart_write(s->channel, offset,
                            exynos4210_uart_regname(offset), val);

    switch (offset) {
    case ULCON:
    case UBRDIV:
    case UFRACVAL:
        s->reg[I_(offset)] = val;
        exynos4210_uart_update_parameters(s);
        break;
    case UFCON:
        s->reg[I_(UFCON)] = val;
        if (val & UFCON_Rx_FIFO_RESET) {
            fifo_reset(&s->rx);
            timer_del(s->fifo_timeout_timer);
            s->rx_since_timeout = false;
            s->reg[I_(UTRSTAT)] &= ~(UTRSTAT_Rx_BUFFER_DATA_READY |
                                     UTRSTAT_Rx_TIMEOUT);
            s->reg[I_(UFCON)] &= ~UFCON_Rx_FIFO_RESET;
            trace_exynos_uart_rx_fifo_reset(s->channel);
        }
        if (val & UFCON_Tx_FIFO_RESET) {
            fifo_reset(&s->tx);
            s->reg[I_(UFCON)] &= ~UFCON_Tx_FIFO_RESET;
            trace_exynos_uart_tx_fifo_reset(s->channel);
        }
        exynos4210_uart_update_dmabusy(s);
        break;

    case UTXH:
        if (qemu_chr_fe_backend_connected(&s->chr)) {
            s->reg[I_(UTRSTAT)] &= ~(UTRSTAT_TRANSMITTER_EMPTY |
                    UTRSTAT_Tx_BUFFER_EMPTY);
            ch = (uint8_t)val;
            /* XXX this blocks entire thread. Rewrite to use
             * qemu_chr_fe_write and background I/O callbacks */
            qemu_chr_fe_write_all(&s->chr, &ch, 1);
            trace_exynos_uart_tx(s->channel, ch);
            s->reg[I_(UTRSTAT)] |= UTRSTAT_TRANSMITTER_EMPTY |
                    UTRSTAT_Tx_BUFFER_EMPTY;
            s->reg[I_(UINTSP)]  |= UINTSP_TXD;
            exynos4210_uart_update_irq(s);
        }
        break;

    case UINTP:
        s->reg[I_(UINTP)] &= ~val;
        s->reg[I_(UINTSP)] &= ~val;
        trace_exynos_uart_intclr(s->channel, s->reg[I_(UINTP)]);
        exynos4210_uart_update_irq(s);
        break;
    case UTRSTAT:
        if (val & UTRSTAT_Rx_TIMEOUT) {
            s->reg[I_(UTRSTAT)] &= ~UTRSTAT_Rx_TIMEOUT;
        }
        /* S5L8720: writing UTRSTAT acknowledges the Tx/Rx interrupt. Clear the
         * corresponding pending source so the (exynos) UINTP line de-asserts,
         * ending the storm; the Tx source re-arms on the next UTXH write. */
        if (s->s5l8720_irq) {
            if (val & UTRSTAT_S5L_Tx_INT) {
                s->reg[I_(UINTSP)] &= ~UINTSP_TXD;
                s->reg[I_(UINTP)]  &= ~UINTSP_TXD;
            }
            if (val & UTRSTAT_S5L_Rx_INT) {
                s->reg[I_(UINTSP)] &= ~UINTSP_RXD;
                s->reg[I_(UINTP)]  &= ~UINTSP_RXD;
            }
            if (val & UTRSTAT_S5L_Err_INT) {
                s->reg[I_(UINTSP)] &= ~UINTSP_ERROR;
                s->reg[I_(UINTP)]  &= ~UINTSP_ERROR;
            }
            exynos4210_uart_update_irq(s);
        }
        break;
    case UERSTAT:
    case UFSTAT:
    case UMSTAT:
    case URXH:
        trace_exynos_uart_ro_write(
                    s->channel, exynos4210_uart_regname(offset), offset);
        break;
    case UINTSP:
        s->reg[I_(UINTSP)]  &= ~val;
        break;
    case UINTM:
        s->reg[I_(UINTM)] = val;
        exynos4210_uart_update_irq(s);
        break;
    case UCON:
        s->reg[I_(offset)] = val;
        /* A mode transition can expose buffered data to DMA, or stop its
         * request. Do not leave the old level latched until the next byte. */
        exynos4210_uart_update_dmabusy(s);
        if (!(val & 3)) {
            timer_del(s->fifo_timeout_timer);
        } else if (s->rx_since_timeout) {
            exynos4210_uart_rx_timeout_set(s);
        }
        exynos4210_uart_update_irq(s);
        break;
    case UMCON:
    default:
        s->reg[I_(offset)] = val;
        break;
    }
}

static uint64_t exynos4210_uart_read(void *opaque, hwaddr offset,
                                  unsigned size)
{
    Exynos4210UartState *s = (Exynos4210UartState *)opaque;
    uint32_t res;

    switch (offset) {
    case UERSTAT: /* Read Only */
        res = s->reg[I_(UERSTAT)];
        s->reg[I_(UERSTAT)] = 0;
        trace_exynos_uart_read(s->channel, offset,
                               exynos4210_uart_regname(offset), res);
        return res;
    case UFSTAT: /* Read Only */
        s->reg[I_(UFSTAT)] = fifo_elements_number(&s->rx) & 0xff;
        if (fifo_empty_elements_number(&s->rx) == 0) {
            s->reg[I_(UFSTAT)] |= UFSTAT_Rx_FIFO_FULL;
            s->reg[I_(UFSTAT)] &= ~0xff;
        }
        trace_exynos_uart_read(s->channel, offset,
                               exynos4210_uart_regname(offset),
                               s->reg[I_(UFSTAT)]);
        return s->reg[I_(UFSTAT)];
    case URXH:
        if (s->reg[I_(UFCON)] & UFCON_FIFO_ENABLE) {
            if (fifo_elements_number(&s->rx)) {
                res = fifo_retrieve(&s->rx);
                trace_exynos_uart_rx(s->channel, res);
                if (!fifo_elements_number(&s->rx)) {
                    s->reg[I_(UTRSTAT)] &= ~UTRSTAT_Rx_BUFFER_DATA_READY;
                } else {
                    s->reg[I_(UTRSTAT)] |= UTRSTAT_Rx_BUFFER_DATA_READY;
                }
            } else {
                trace_exynos_uart_rx_error(s->channel);
                s->reg[I_(UINTSP)] |= UINTSP_ERROR;
                exynos4210_uart_update_irq(s);
                res = 0;
            }
        } else {
            s->reg[I_(UTRSTAT)] &= ~UTRSTAT_Rx_BUFFER_DATA_READY;
            res = s->reg[I_(URXH)];
        }
        qemu_chr_fe_accept_input(&s->chr);
        exynos4210_uart_update_dmabusy(s);
        trace_exynos_uart_read(s->channel, offset,
                               exynos4210_uart_regname(offset), res);
        return res;
    case UTXH:
        trace_exynos_uart_wo_read(s->channel, exynos4210_uart_regname(offset),
                                  offset);
        break;
    default:
        trace_exynos_uart_read(s->channel, offset,
                               exynos4210_uart_regname(offset),
                               s->reg[I_(offset)]);
        return s->reg[I_(offset)];
    }

    trace_exynos_uart_read(s->channel, offset, exynos4210_uart_regname(offset),
                           0);
    return 0;
}

static const MemoryRegionOps exynos4210_uart_ops = {
    .read = exynos4210_uart_read,
    .write = exynos4210_uart_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .max_access_size = 4,
        .unaligned = false
    },
};

static int exynos4210_uart_can_receive(void *opaque)
{
    Exynos4210UartState *s = (Exynos4210UartState *)opaque;

    if (s->reg[I_(UFCON)] & UFCON_FIFO_ENABLE) {
        return fifo_empty_elements_number(&s->rx);
    } else {
        return !(s->reg[I_(UTRSTAT)] & UTRSTAT_Rx_BUFFER_DATA_READY);
    }
}

static void exynos4210_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    Exynos4210UartState *s = (Exynos4210UartState *)opaque;
    int i;

    if (s->reg[I_(UFCON)] & UFCON_FIFO_ENABLE) {
        if (fifo_empty_elements_number(&s->rx) < size) {
            size = fifo_empty_elements_number(&s->rx);
            s->reg[I_(UINTSP)] |= UINTSP_ERROR;
        }
        for (i = 0; i < size; i++) {
            fifo_store(&s->rx, buf[i]);
        }
        exynos4210_uart_rx_timeout_set(s);
    } else {
        s->reg[I_(URXH)] = buf[0];
    }
    s->reg[I_(UTRSTAT)] |= UTRSTAT_Rx_BUFFER_DATA_READY;
    s->rx_since_timeout = true;

    exynos4210_uart_update_irq(s);
}


static void exynos4210_uart_event(void *opaque, QEMUChrEvent event)
{
    Exynos4210UartState *s = (Exynos4210UartState *)opaque;

    if (event == CHR_EVENT_BREAK) {
        /* When the RxDn is held in logic 0, then a null byte is pushed into the
         * fifo */
        fifo_store(&s->rx, '\0');
        s->reg[I_(UERSTAT)] |= UERSTAT_BREAK;
        exynos4210_uart_update_irq(s);
    }
}


static void exynos4210_uart_reset(DeviceState *dev)
{
    Exynos4210UartState *s = EXYNOS4210_UART(dev);
    int i;

    for (i = 0; i < ARRAY_SIZE(exynos4210_uart_regs); i++) {
        s->reg[I_(exynos4210_uart_regs[i].offset)] =
                exynos4210_uart_regs[i].reset_value;
    }

    fifo_reset(&s->rx);
    fifo_reset(&s->tx);
    s->rx_since_timeout = false;

    trace_exynos_uart_rxsize(s->channel, s->rx.size);
}

static int exynos4210_uart_post_load(void *opaque, int version_id)
{
    Exynos4210UartState *s = (Exynos4210UartState *)opaque;

    exynos4210_uart_update_parameters(s);
    exynos4210_uart_rx_timeout_set(s);

    return 0;
}

static const VMStateDescription vmstate_exynos4210_uart_fifo = {
    .name = "exynos4210.uart.fifo",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(sp, Exynos4210UartFIFO),
        VMSTATE_UINT32(rp, Exynos4210UartFIFO),
        VMSTATE_VBUFFER_UINT32(data, Exynos4210UartFIFO, 1, NULL, size),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_exynos4210_uart = {
    .name = "exynos4210.uart",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = exynos4210_uart_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(rx, Exynos4210UartState, 1,
                       vmstate_exynos4210_uart_fifo, Exynos4210UartFIFO),
        VMSTATE_UINT32_ARRAY(reg, Exynos4210UartState,
                             EXYNOS4210_UART_REGS_MEM_SIZE / sizeof(uint32_t)),
        VMSTATE_END_OF_LIST()
    }
};

DeviceState *exynos4210_uart_create(hwaddr addr,
                                    int fifo_size,
                                    int channel,
                                    Chardev *chr,
                                    qemu_irq irq, bool s5l8720_irq)
{
    DeviceState  *dev;
    SysBusDevice *bus;

    dev = qdev_new(TYPE_EXYNOS4210_UART);

    qdev_prop_set_bit(dev, "s5l8720-irq", s5l8720_irq);
    qdev_prop_set_chr(dev, "chardev", chr);
    qdev_prop_set_uint32(dev, "channel", channel);
    qdev_prop_set_uint32(dev, "rx-size", fifo_size);
    qdev_prop_set_uint32(dev, "tx-size", fifo_size);

    bus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(bus, &error_fatal);
    if (addr != (hwaddr)-1) {
        sysbus_mmio_map(bus, 0, addr);
    }
    sysbus_connect_irq(bus, 0, irq);

    return dev;
}

static void exynos4210_uart_init(Object *obj)
{
    SysBusDevice *dev = SYS_BUS_DEVICE(obj);
    Exynos4210UartState *s = EXYNOS4210_UART(dev);

    s->wordtime = NANOSECONDS_PER_SECOND * 10 / 9600;

    /* memory mapping */
    memory_region_init_io(&s->iomem, obj, &exynos4210_uart_ops, s,
                          "exynos4210.uart", EXYNOS4210_UART_REGS_MEM_SIZE);
    sysbus_init_mmio(dev, &s->iomem);

    sysbus_init_irq(dev, &s->irq);
    sysbus_init_irq(dev, &s->dmairq);
    sysbus_init_irq(dev, &s->rxdmareq);
}

static void exynos4210_uart_realize(DeviceState *dev, Error **errp)
{
    Exynos4210UartState *s = EXYNOS4210_UART(dev);

    s->fifo_timeout_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                         exynos4210_uart_timeout_int, s);

    qemu_chr_fe_set_handlers(&s->chr, exynos4210_uart_can_receive,
                             exynos4210_uart_receive, exynos4210_uart_event,
                             NULL, s, NULL, true);
}

static const Property exynos4210_uart_properties[] = {
    DEFINE_PROP_BOOL("s5l8720-irq", Exynos4210UartState, s5l8720_irq, false),
    DEFINE_PROP_CHR("chardev", Exynos4210UartState, chr),
    DEFINE_PROP_UINT32("channel", Exynos4210UartState, channel, 0),
    DEFINE_PROP_UINT32("rx-size", Exynos4210UartState, rx.size, 16),
    DEFINE_PROP_UINT32("tx-size", Exynos4210UartState, tx.size, 16),
};

static void exynos4210_uart_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = exynos4210_uart_realize;
    device_class_set_legacy_reset(dc, exynos4210_uart_reset);
    device_class_set_props(dc, exynos4210_uart_properties);
    dc->vmsd = &vmstate_exynos4210_uart;
}

static const TypeInfo exynos4210_uart_info = {
    .name          = TYPE_EXYNOS4210_UART,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Exynos4210UartState),
    .instance_init = exynos4210_uart_init,
    .class_init    = exynos4210_uart_class_init,
};

static void exynos4210_uart_register(void)
{
    type_register_static(&exynos4210_uart_info);
}

type_init(exynos4210_uart_register)
