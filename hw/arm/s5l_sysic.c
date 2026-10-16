#include "hw/arm/s5l_sysic.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties.h"

static uint64_t s5l_sysic_read(void *opaque, hwaddr addr, unsigned size)
{
    S5LSYSICState *s = (S5LSYSICState *) opaque;

    //fprintf(stderr, "%s: offset = 0x%08x\n", __func__, addr);

    switch (addr) {
        case POWER_ID:
            return s->power_id;
        case POWER_SETSTATE:
        case POWER_STATE:
            return s->power_state;
        case 0x7a:
        case 0x7c:
            return 1;
        case GPIO_INTLEVEL ... (GPIO_INTLEVEL + GPIO_NUMINTGROUPS * 4):
        {
            uint8_t group = (addr - GPIO_INTLEVEL) / 4;
            return s->gpio_int_level[group];
        }
        case GPIO_INTSTAT ... (GPIO_INTSTAT + GPIO_NUMINTGROUPS * 4):
        {
            uint8_t group = (addr - GPIO_INTSTAT) / 4;
            return s->gpio_int_status[group];
        }
        case GPIO_INTEN ... (GPIO_INTEN + GPIO_NUMINTGROUPS * 4):
        {
            uint8_t group = (addr - GPIO_INTEN) / 4;
            return s->gpio_int_enabled[group];
        }
        case GPIO_INTTYPE ... (GPIO_INTTYPE + GPIO_NUMINTGROUPS * 4):
        {
            uint8_t group = (addr - GPIO_INTTYPE) / 4;
            return s->gpio_int_type[group];
        }
      default:
        break;
    }
    return 0;
}

// Drives the interrupt line of a GPIO interrupt group, which is asserted while any of its enabled interrupts is pending.
// Enabled level-triggered interrupts remain pending as long as their input line is asserted. Note that the kernel keeps
// handling interrupts until the status register reads zero, so disabled interrupts must not become pending again.
static void s5l_sysic_update_gpio_irq(S5LSYSICState *s, int group)
{
    s->gpio_int_status[group] |= s->gpio_int_line[group] & s->gpio_int_type[group] & s->gpio_int_enabled[group];
    qemu_set_irq(s->gpio_irqs[group], (s->gpio_int_status[group] & s->gpio_int_enabled[group]) != 0);
}

static void s5l_sysic_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    S5LSYSICState *s = (S5LSYSICState *) opaque;

    //fprintf(stderr, "%s: writing 0x%08x to 0x%08x\n", __func__, val, addr);

    switch (addr) {
        case POWER_ID:
            s->power_id = val;
            break;
        case POWER_ONCTRL:
            if (s->power_off_bits) {
                s->power_state &= ~val;
                break;
            }
            if((val & 0x20) != 0 || (val & 0x4) != 0 || (val & POWER_ID_ADM) != 0) { break; } // make sure that we do not record the 'on' state of some devices so it appears like they are turned on immediately.
            s->power_state = val;
            break;
        case POWER_OFFCTRL:
            if (s->power_off_bits) {
                s->power_state |= val;
                break;
            }
            s->power_state = val;
            break;
        case GPIO_INTLEVEL ... (GPIO_INTLEVEL + GPIO_NUMINTGROUPS * 4):
        {
            break;
        }
        case GPIO_INTSTAT ... (GPIO_INTSTAT + GPIO_NUMINTGROUPS * 4):
        {
            uint8_t group = (addr - GPIO_INTSTAT) / 4;

            // acknowledge the interrupts and clear the corresponding bits
            s->gpio_int_status[group] = s->gpio_int_status[group] & ~val;

            s5l_sysic_update_gpio_irq(s, group);

            break;
        }
        case GPIO_INTEN ... (GPIO_INTEN + GPIO_NUMINTGROUPS * 4):
        {
            uint8_t group = (addr - GPIO_INTEN) / 4;
            s->gpio_int_enabled[group] = val;
            s5l_sysic_update_gpio_irq(s, group);
            break;
        }
        case GPIO_INTTYPE ... (GPIO_INTTYPE + GPIO_NUMINTGROUPS * 4):
        {
            uint8_t group = (addr - GPIO_INTTYPE) / 4;
            s->gpio_int_type[group] = val;
            break;
        }
        default:
            break;
    }
}

static const MemoryRegionOps s5l_sysic_ops = {
    .read = s5l_sysic_read,
    .write = s5l_sysic_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

// Sets the input line of GPIO interrupt n, e.g., when a peripheral asserts or deasserts its interrupt line.
static void s5l_sysic_gpio_irq(void *opaque, int n, int level)
{
    S5LSYSICState *s = S5L_SYSIC(opaque);
    int group = n / GPIO_NUMINTSPERGROUP;
    uint32_t bit = 1 << (n % GPIO_NUMINTSPERGROUP);

    if (level) {
        s->gpio_int_line[group] |= bit;
        if (!(s->gpio_int_type[group] & bit)) {
            // edge-triggered interrupts are latched
            s->gpio_int_status[group] |= bit;
        }
        s5l_sysic_update_gpio_irq(s, group);
    } else {
        s->gpio_int_line[group] &= ~bit;
    }
}

static void s5l_sysic_init(Object *obj)
{
    S5LSYSICState *s = S5L_SYSIC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &s5l_sysic_ops, s, TYPE_S5L_SYSIC, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    for(int grp = 0; grp < GPIO_NUMINTGROUPS; grp++) {
        sysbus_init_irq(sbd, &s->gpio_irqs[grp]);
    }
    qdev_init_gpio_in(DEVICE(obj), s5l_sysic_gpio_irq, GPIO_NUMINTGROUPS * GPIO_NUMINTSPERGROUP);
}

static const VMStateDescription vmstate_s5l_sysic = {
    .name = TYPE_S5L_SYSIC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(power_id, S5LSYSICState),
        VMSTATE_UINT32(power_state, S5LSYSICState),
        VMSTATE_UINT32_ARRAY(gpio_int_level, S5LSYSICState, GPIO_NUMINTGROUPS),
        VMSTATE_UINT32_ARRAY(gpio_int_status, S5LSYSICState, GPIO_NUMINTGROUPS),
        VMSTATE_UINT32_ARRAY(gpio_int_enabled, S5LSYSICState, GPIO_NUMINTGROUPS),
        VMSTATE_UINT32_ARRAY(gpio_int_type, S5LSYSICState, GPIO_NUMINTGROUPS),
        VMSTATE_UINT32_ARRAY(gpio_int_line, S5LSYSICState, GPIO_NUMINTGROUPS),
        VMSTATE_END_OF_LIST()
    }
};

static const Property s5l_sysic_properties[] = {
    DEFINE_PROP_BOOL("power-off-bits", S5LSYSICState, power_off_bits, false),
};

static void s5l_sysic_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_s5l_sysic;
    device_class_set_props(dc, s5l_sysic_properties);
}

static const TypeInfo s5l_sysic_type_info = {
    .name = TYPE_S5L_SYSIC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5LSYSICState),
    .instance_init = s5l_sysic_init,
    .class_init = s5l_sysic_class_init,
};

static void s5l_sysic_register_types(void)
{
    type_register_static(&s5l_sysic_type_info);
}

type_init(s5l_sysic_register_types)
