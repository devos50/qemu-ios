#ifndef HW_WM8758_H
#define HW_WM8758_H

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/arm/ipod_touch_i2s.h"

#define TYPE_WM8758 "wm8758"
OBJECT_DECLARE_SIMPLE_TYPE(WM8758State, WM8758)

#define WM8758_NUM_REGS 0x40

/*
 * Registers, as AppleWM8758Audio uses them (the WM8978 family's layout). Registers hold 9 bits.
 */
#define WM8758_RESET      0x00 // any write resets the registers
#define WM8758_PWR2       0x02 // bit 7: LOUT1 (headphone left) on, bit 8: ROUT1 on
#define WM8758_PWR3       0x03 // bit 0/1: left/right DAC on, bit 2/3: left/right mixer on, bit 6/5: LOUT2/ROUT2 on
#define WM8758_CLK_GEN    0x06 // bit 8: SYSCLK from the PLL, bits 7:5: MCLKDIV
#define WM8758_DAC_CTL    0x0A // bit 6: soft mute
#define WM8758_DAC_VOL_L  0x0B // 0.5 dB steps, 0xff is 0 dB, 0 mutes
#define WM8758_DAC_VOL_R  0x0C
#define WM8758_PLL_N      0x24 // bit 4: MCLK / 2 into the PLL, bits 3:0: N
#define WM8758_PLL_K1     0x25 // bits 5:0: K[23:18]
#define WM8758_PLL_K2     0x26 // K[17:9]
#define WM8758_PLL_K3     0x27 // K[8:0]
#define WM8758_MIX_L      0x32 // bit 0: left DAC into the left output mixer
#define WM8758_MIX_R      0x33
#define WM8758_OUT1_VOL_L 0x34 // headphones; bit 6: mute, bits 5:0: gain, 0x39 is 0 dB, 1 dB steps
#define WM8758_OUT1_VOL_R 0x35
#define WM8758_OUT2_VOL_L 0x36 // the piezo clicker, same layout
#define WM8758_OUT2_VOL_R 0x37

#define WM8758_VOL_MUTE   0x40
#define WM8758_VOL_GAIN   0x3F
#define WM8758_VOL_0DB    0x39

typedef struct WM8758State {
    I2CSlave i2c;
    IPodTouchI2SState *i2s; // carries the samples this codec plays
    uint32_t mclk_hz;        // the master clock the S5L8900 gives the codec
    uint16_t regs[WM8758_NUM_REGS];
    uint8_t buf[2];          // the write being received: register address and bit 8, then bits 7:0
    uint8_t len;
} WM8758State;

#endif
