#include "hw/arm/s5l_gpio.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

static void s5l_gpio_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    //fprintf(stderr, "%s: writing 0x%08x to 0x%08x\n", __func__, value, addr);
    S5LGPIOState *s = (struct S5LGPIOState *) opaque;

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

static uint64_t s5l_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    //printf("%s: read from location 0x%08x\n", __func__, addr);
    S5LGPIOState *s = (struct S5LGPIOState *) opaque;

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
    .read = s5l_gpio_read,
    .write = s5l_gpio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void s5l_gpio_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(sbd);
    S5LGPIOState *s = S5L_GPIO(dev);

    memory_region_init_io(&s->iomem, obj, &gpio_ops, s, "gpio", 0x1000);
    qdev_init_gpio_out(dev, s->outputs, ARRAY_SIZE(s->outputs));
}

static const VMStateDescription vmstate_s5l_gpio = {
    .name = TYPE_S5L_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(gpio_state, S5LGPIOState, NUM_GPIO_PADS),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_s5l_gpio;
}

static const TypeInfo s5l_gpio_info = {
    .name          = TYPE_S5L_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5LGPIOState),
    .instance_init = s5l_gpio_init,
    .class_init    = s5l_gpio_class_init,
};

static void s5l_gpio_register_types(void)
{
    type_register_static(&s5l_gpio_info);
}

type_init(s5l_gpio_register_types)