#include "hw/arm/atv4_gpio.h"

static void atv4_gpio_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    //fprintf(stderr, "%s: writing 0x%08x to 0x%08x\n", __func__, value, addr);
    ATV4GPIOState *s = (struct ATV4GPIOState *) opaque;

    switch(addr) {
      default:
        break;
    }
}

static uint64_t atv4_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    //printf("%s: read from location 0x%08x\n", __func__, addr);
    ATV4GPIOState *s = (struct ATV4GPIOState *) opaque;

    switch(addr) {
        default:
            break;
    }

    return 0;
}

static const MemoryRegionOps gpio_ops = {
    .read = atv4_gpio_read,
    .write = atv4_gpio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void atv4_gpio_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(sbd);
    ATV4GPIOState *s = ATV4_GPIO(dev);

    memory_region_init_io(&s->iomem, obj, &gpio_ops, s, "gpio", 0x1000);
}

static void atv4_gpio_class_init(ObjectClass *klass, void *data)
{

}

static const TypeInfo atv4_gpio_info = {
    .name          = TYPE_ATV4_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ATV4GPIOState),
    .instance_init = atv4_gpio_init,
    .class_init    = atv4_gpio_class_init,
};

static void atv4_gpio_register_types(void)
{
    type_register_static(&atv4_gpio_info);
}

type_init(atv4_gpio_register_types)