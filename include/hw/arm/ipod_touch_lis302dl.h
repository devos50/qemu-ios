#ifndef HW_LIS302DL_H
#define HW_LIS302DL_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"

#define TYPE_LIS302DL                 "lis302dl"
OBJECT_DECLARE_SIMPLE_TYPE(LIS302DLState, LIS302DL)

#define ACCEL_WHOAMI	0x0F
#define ACCEL_CTRL_REG1 0x20
#define ACCEL_CTRL_REG2 0x21
#define ACCEL_CTRL_REG3 0x22
#define ACCEL_STATUS    0x27
#define ACCEL_OUT_X     0x29
#define ACCEL_OUT_Y     0x2B
#define ACCEL_OUT_Z     0x2D

#define ACCEL_WHOAMI_VALUE	0x3B

// CTRL_REG1 bits
#define ACCEL_CTRL1_XEN 0x01
#define ACCEL_CTRL1_YEN 0x02
#define ACCEL_CTRL1_ZEN 0x04
#define ACCEL_CTRL1_PD  0x40 // power up (0 = power-down mode)
#define ACCEL_CTRL1_RESET_VALUE (ACCEL_CTRL1_XEN | ACCEL_CTRL1_YEN | ACCEL_CTRL1_ZEN)

// STATUS_REG bits
#define ACCEL_STATUS_ZYXDA 0x08 // new X, Y and Z data available

// The first byte of a write is the register address; setting this bit enables auto-increment
#define ACCEL_SUBADDR_AUTOINC 0x80

// Sensitivity in the +/- 2g range (typical): 18 mg per LSB
#define ACCEL_MG_PER_LSB 18

// Output noise of a real sensor at rest, in LSB either way. AppleLIS302DL only reports a sample when it differs from
// the previous one, so a perfectly still sensor would produce a single event.
#define ACCEL_NOISE_LSB 1

typedef struct LIS302DLState {
	I2CSlave i2c;
	uint8_t reg;         // current register pointer
	bool autoinc;        // auto-increment the register pointer after each access
	bool reg_written;    // whether the register address of this write transfer has been received
	uint8_t ctrl_reg1;
	uint8_t ctrl_reg2;
	uint8_t ctrl_reg3;
	int32_t accel_mg[3]; // acceleration along the X, Y and Z axes, in mg
	uint32_t noise_seed; // state of the noise generator
	int orientation;     // index of the orientation lis302dl_rotate last selected, -1 for lying flat
} LIS302DLState;

const char *lis302dl_rotate(LIS302DLState *s);

#endif
