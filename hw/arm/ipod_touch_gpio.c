#include "hw/arm/ipod_touch_gpio.h"
#include "hw/irq.h"

static void s5l8900_gpio_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    //fprintf(stderr, "%s: writing 0x%08x to 0x%08x\n", __func__, value, addr);
    IPodTouchGPIOState *s = (struct IPodTouchGPIOState *) opaque;

    switch(addr) {
      case GPIO_FSEL: {
        // bits 16-23: pad, bits 8-10: pin, bits 0-3: function, where 0xE/0xF configure the pin as an output driving 0/1
        uint32_t pad = (value >> 16) & 0xFF;
        uint32_t pin = (value >> 8) & 7;
        uint32_t function = value & 0xF;
        if (pad < NUM_GPIO_OUT_PADS && (function == 0xE || function == 0xF)) {
            qemu_set_irq(s->outputs[(pad << 3) | pin], function & 1);
        }
        break;
      }
      default:
        break;
    }
}

static uint64_t s5l8900_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    //printf("%s: read from location 0x%08x\n", __func__, addr);
    IPodTouchGPIOState *s = (struct IPodTouchGPIOState *) opaque;

    switch(addr) {
        case 0x4:
            return 0;
        case 0x24 ... 0x184:
            return s->gpio_state[GPIOADDR2PAD(addr)];
        default:
            break;
    }

    return 0;
}

bool gpio_is_on(uint32_t *state, uint32_t gpio)
{
    return (state[GPIO2PAD(gpio)] & (1 << GPIO2PIN(gpio)));
}

bool gpio_is_off(uint32_t *state, uint32_t gpio)
{
    return !gpio_is_on(state, gpio);
}

void gpio_set_on(uint32_t *state, uint32_t gpio)
{
    state[GPIO2PAD(gpio)] |= (1 << GPIO2PIN(gpio));
}

void gpio_set_off(uint32_t *state, uint32_t gpio)
{
    state[GPIO2PAD(gpio)] &= ~(1 << GPIO2PIN(gpio));
}

static const MemoryRegionOps gpio_ops = {
    .read = s5l8900_gpio_read,
    .write = s5l8900_gpio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void s5l8900_gpio_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(sbd);
    IPodTouchGPIOState *s = IPOD_TOUCH_GPIO(dev);

    memory_region_init_io(&s->iomem, obj, &gpio_ops, s, "gpio", 0x1000);
    qdev_init_gpio_out(dev, s->outputs, ARRAY_SIZE(s->outputs));
}

static void s5l8900_gpio_class_init(ObjectClass *klass, void *data)
{

}

static const TypeInfo ipod_touch_gpio_info = {
    .name          = TYPE_IPOD_TOUCH_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchGPIOState),
    .instance_init = s5l8900_gpio_init,
    .class_init    = s5l8900_gpio_class_init,
};

static void ipod_touch_machine_types(void)
{
    type_register_static(&ipod_touch_gpio_info);
}

type_init(ipod_touch_machine_types)