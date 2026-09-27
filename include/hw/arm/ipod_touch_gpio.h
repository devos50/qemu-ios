#ifndef IPOD_TOUCH_GPIO_H
#define IPOD_TOUCH_GPIO_H

#include <math.h>
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"

#define TYPE_IPOD_TOUCH_GPIO                "ipodtouch.gpio"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchGPIOState, IPOD_TOUCH_GPIO)

#define GPIO_BUTTON_POWER   0xC02
#define GPIO_BUTTON_HOME    0xC01
#define GPIO_BUTTON_VOLUP   0x902
#define GPIO_BUTTON_VOLDOWN 0xC00
#define GPIO_FORCE_DFU      0xB03

#define GPIO_BUTTON_POWER_IRQ   0x7A
#define GPIO_BUTTON_HOME_IRQ    0x79
#define GPIO_BUTTON_VOLUP_IRQ   0x78
#define GPIO_BUTTON_VOLDOWN_IRQ 0x62
#define GPIO_LIGHTSENSOR_IRQ    0x25
#define GPIO_I2S0_IRQ           0x2C // i2s0 frame sync

#define NUM_GPIO_PADS 0xC
#define NUM_GPIO_PINS 0x20

// Output lines, indexed by (pad << 3) | pin, driven through the pin configuration register
#define GPIO_FSEL           0x1E0
#define NUM_GPIO_OUT_PADS   0x10
#define GPIO_OUT_INDEX(gpio) ((GPIO2PAD(gpio) << 3) | GPIO2PIN(gpio))

#define GPIO_NOR_CS_SPI0    0x000
#define GPIO_NOR_CS_SPI1    0x406

#define GPIO2PIN(gpio)       ((gpio) & 7)
#define GPIO2PAD(gpio)       (((gpio) >> 8) & 0xFF)
#define GPIOADDR2PAD(addr)   (addr - 0x4) / 0x20

typedef struct IPodTouchGPIOState
{
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t gpio_state[NUM_GPIO_PADS];
    qemu_irq outputs[NUM_GPIO_OUT_PADS * 8];
} IPodTouchGPIOState;

bool gpio_is_on(uint32_t *state, uint32_t gpio);
bool gpio_is_off(uint32_t *state, uint32_t gpio);
void gpio_set_on(uint32_t *state, uint32_t gpio);
void gpio_set_off(uint32_t *state, uint32_t gpio);

#endif