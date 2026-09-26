#ifndef HW_ISL29003DL_H
#define HW_ISL29003DL_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"

#define TYPE_ISL29003DL                 "isl29003dl"
OBJECT_DECLARE_SIMPLE_TYPE(ISL29003DLState, ISL29003DL)

#define LIGHTSENSOR_COMMAND    0x00
#define LIGHTSENSOR_CONTROL    0x01
#define LIGHTSENSOR_IT_HI      0x02
#define LIGHTSENSOR_IT_LO      0x03
#define LIGHTSENSOR_LSB_SENSOR 0x04
#define LIGHTSENSOR_MSB_SENSOR 0x05
#define LIGHTSENSOR_LSB_TIMER  0x06
#define LIGHTSENSOR_MSB_TIMER  0x07
#define LIGHTSENSOR_NUM_REGS   0x08

// COMMAND register bits
#define LIGHTSENSOR_CMD_ENABLE     0x80
#define LIGHTSENSOR_CMD_ADC_PD     0x40 // ADC power-down
#define LIGHTSENSOR_CMD_MODE_MASK  0x0C
#define LIGHTSENSOR_CMD_MODE_D1    0x00 // diode 1 (visible + infrared)
#define LIGHTSENSOR_CMD_MODE_D2    0x04 // diode 2 (infrared only)
#define LIGHTSENSOR_CMD_MODE_D1_D2 0x08 // diode 1 - diode 2
#define LIGHTSENSOR_CMD_WIDTH_MASK 0x03 // ADC resolution: 16, 12, 8 or 4 bits

// CONTROL register bits
#define LIGHTSENSOR_CTRL_INT_FLAG   0x20
#define LIGHTSENSOR_CTRL_RANGE_MASK 0x03

// The kernel derives an ADC clock of ~65 kHz from the external scaling resistor on this device (499 kOhm).
#define LIGHTSENSOR_ADC_CLOCK_HZ 65000

typedef struct ISL29003DLState {
	I2CSlave i2c;
	qemu_irq irq;
	QEMUTimer *timer;
	uint8_t reg;         // current register pointer
	bool reg_written;    // whether the register address of this write transfer has been received
	uint8_t regs[LIGHTSENSOR_NUM_REGS];
	uint32_t lux;        // ambient light level
} ISL29003DLState;

#endif
