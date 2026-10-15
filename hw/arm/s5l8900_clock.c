/*
 * The clock and power manager of the S5L8900 (CLOCK1). The PLLs lock at once; everything else reads back what was
 * written, from the values of a real iPod touch 1G after reset (from the ipod_touch_1g branch of qemu-ios).
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/arm/s5l8900_clock.h"

#define PLL_ALL_LOCKED  0xf

static uint64_t s5l8900_clock_read(void *opaque, hwaddr addr, unsigned size)
{
    S5L8900ClockState *s = opaque;

    if (addr == S5L8900_CLOCK_PLLLOCK) {
        return PLL_ALL_LOCKED;
    }
    return s->regs[addr / 4];
}

static void s5l8900_clock_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    S5L8900ClockState *s = opaque;
    s->regs[addr / 4] = val;
}

static const MemoryRegionOps s5l8900_clock_ops = {
    .read = s5l8900_clock_read,
    .write = s5l8900_clock_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void s5l8900_clock_reset(DeviceState *dev)
{
    S5L8900ClockState *s = S5L8900_CLOCK(dev);

    memset(s->regs, 0, sizeof(s->regs));
    // clock PLL 1, memory divisor 2
    s->regs[S5L8900_CLOCK_CONFIG0 / 4] = (1 << 12) | (1 << 24) | (2 << 16);
    // bus PLL 1, bus divisor 3, peripheral factor 1, and bits whose meaning is unknown
    s->regs[S5L8900_CLOCK_CONFIG1 / 4] = (1 << 12) | (1 << 24) | (3 << 16) | (1 << 8) | 3 | (1 << 20) | (1 << 14) |
                                         (1 << 28) | (1 << 30);
    // peripheral PLL 3, display divisor 1
    s->regs[S5L8900_CLOCK_CONFIG2 / 4] = (3 << 28) | (1 << 24) | (1 << 16);
    // MDIV << 8, PDIV << 24, SDIV
    s->regs[S5L8900_CLOCK_PLL0CON / 4] = (80 << 8) | (8 << 24) | 0;
    s->regs[S5L8900_CLOCK_PLL1CON / 4] = (103 << 8) | (6 << 24) | 0;
    s->regs[S5L8900_CLOCK_PLL2CON / 4] = (156 << 8) | (53 << 24) | 2;
    s->regs[S5L8900_CLOCK_PLL3CON / 4] = (72 << 8) | (8 << 24) | 1;
    s->regs[S5L8900_CLOCK_PLLMODE / 4] = 0x000a003a;
}

static void s5l8900_clock_init(Object *obj)
{
    S5L8900ClockState *s = S5L8900_CLOCK(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8900_clock_ops, s, TYPE_S5L8900_CLOCK, S5L8900_CLOCK_REGS);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_s5l8900_clock = {
    .name = TYPE_S5L8900_CLOCK,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, S5L8900ClockState, S5L8900_CLOCK_REGS / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8900_clock_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, s5l8900_clock_reset);
    dc->vmsd = &vmstate_s5l8900_clock;
}

static const TypeInfo s5l8900_clock_info = {
    .name          = TYPE_S5L8900_CLOCK,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900ClockState),
    .instance_init = s5l8900_clock_init,
    .class_init    = s5l8900_clock_class_init,
};

static void s5l8900_clock_register_types(void)
{
    type_register_static(&s5l8900_clock_info);
}

type_init(s5l8900_clock_register_types)
