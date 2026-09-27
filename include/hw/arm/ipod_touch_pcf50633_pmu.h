#ifndef HW_PCF50633_PMU_H
#define HW_PCF50633_PMU_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "time.h"

#define TYPE_PCF50633                 "pcf50633"
OBJECT_DECLARE_SIMPLE_TYPE(Pcf50633State, PCF50633)

// Charger status, read by the kernel's D1759 power source driver as 3 bytes
#define PMU_STATUS_A 0x04
#define PMU_STATUS_A_USB_PRESENT (1 << 3)

#define PMU_DSBL1 0x30	
#define PMU_MBCS1 0x4B
#define PMU_ADCC1 0x57

// RTC of the D1759 the kernel drives: the time is a little endian seconds
// counter plus an offset that the kernel keeps in scratch registers.
#define PMU_RTC_COUNT 0x5C
#define PMU_RTC_OFFSET 0x64

typedef struct Pcf50633State {
	I2CSlave i2c;
	uint32_t cmd; // register pointer
	bool pointer_set;
	bool usb_present;
	uint32_t rtc_count;
	uint8_t rtc_offset[4];
} Pcf50633State;

#endif
