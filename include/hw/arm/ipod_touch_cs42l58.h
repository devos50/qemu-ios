#ifndef HW_CS42L58_H
#define HW_CS42L58_H

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/arm/ipod_touch_i2s.h"

#define TYPE_CS42L58 "cs42l58"
OBJECT_DECLARE_SIMPLE_TYPE(CS42L58State, CS42L58)

#define CS42L58_NUM_REGS 0x80
#define CS42L58_MAP_INCR 0x80 // in the register address byte: auto-increment the address

/*
 * Registers, as AppleCS42L58Audio uses them. There is no public datasheet: the meanings are inferred from the driver.
 */
#define CS42L58_CHIP_ID   0x01 // bits 2:0 revision; revision 0 makes the driver write an unlock sequence
#define CS42L58_PWR_CTL1  0x02 // bit 0: power down
#define CS42L58_PWR_OUT   0x03 // output power, 2-bit fields (11 off, 10 on, 0x pin controlled): 7:4 path 1, 3:0 path 2
#define CS42L58_CLK_CTL   0x05 // bits 4:0: sample rate
#define CS42L58_PB_CTL    0x0F // bits 1:0: mute channel B/A (set together)
#define CS42L58_VOL_PATH1 0x1A // speaker path volume
#define CS42L58_VOL_PATH2 0x1C // headphone path volume

#define CS42L58_PWR_CTL1_PDN  0x01
#define CS42L58_CLK_RATE_MASK 0x1F
#define CS42L58_PB_CTL_MUTE_A 0x01
#define CS42L58_PB_CTL_MUTE_B 0x02
#define CS42L58_VOL_MUTE      0x80 // otherwise bits 6:0 are the gain in dB, signed (-60..+12)

typedef struct CS42L58State {
    I2CSlave i2c;
    IPodTouchI2SState *i2s; // carries the samples this codec plays
    int32_t speaker_gain_db; // gain of the external speaker amplifier on path 1
    uint8_t regs[CS42L58_NUM_REGS];
    uint8_t map;       // register address
    bool incr;         // auto-increment after each access
    bool map_written;  // whether the register address of this write transfer has been received
} CS42L58State;

#endif
