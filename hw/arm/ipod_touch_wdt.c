/*
 * S5L8720 watchdog timer.
 *
 * The kernel driver (AppleS5L8900XWatchDogTimer) only writes CTRL:
 *  - 0x001F4A00 to kick: enable, prescaler 16, clock select 4, clear the count. It kicks every 2^25 input clocks, i.e.,
 *    it expects this setting to expire after 2^26 input clocks.
 *  - 0x00000000 to disable.
 *  - 0x00100000 to restart the device (enable with the shortest timeout), used by PEHaltRestart for restarts and panics.
 * The split of the 2^26 divider between clock select and counter width is a guess; the product matches the driver.
 */
#include "hw/arm/ipod_touch_wdt.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "sysemu/runstate.h"

static int64_t ipod_touch_wdt_tick_ns(IPodTouchWDTState *s)
{
    uint32_t pre = (s->ctrl >> WDT_CTRL_PRE_SHIFT) & WDT_CTRL_PRE_MASK;
    uint32_t cs = (s->ctrl >> WDT_CTRL_CS_SHIFT) & WDT_CTRL_CS_MASK;
    return muldiv64((uint64_t)(pre + 1) << cs, NANOSECONDS_PER_SECOND, s->clock_hz);
}

static uint32_t ipod_touch_wdt_count(IPodTouchWDTState *s)
{
    if (!(s->ctrl & WDT_CTRL_ENABLE)) {
        return 0;
    }
    int64_t ticks = (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->start_ns) / ipod_touch_wdt_tick_ns(s);
    return MIN(ticks, WDT_COUNT_MAX);
}

static void ipod_touch_wdt_update_timer(IPodTouchWDTState *s)
{
    if (s->ctrl & WDT_CTRL_ENABLE) {
        timer_mod(s->timer, s->start_ns + WDT_COUNT_MAX * ipod_touch_wdt_tick_ns(s));
    } else {
        timer_del(s->timer);
    }
}

static void ipod_touch_wdt_expired(void *opaque)
{
    IPodTouchWDTState *s = opaque;

    if (s->ctrl & WDT_CTRL_INT_EN) {
        qemu_irq_raise(s->irq);
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR, "%s: watchdog expired (ctrl 0x%08x), resetting the system\n", __func__, s->ctrl);
    s->ctrl = 0;
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

static uint64_t ipod_touch_wdt_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchWDTState *s = opaque;

    switch (addr) {
    case WDT_CTRL:
        return s->ctrl;
    case WDT_CNT:
        return ipod_touch_wdt_count(s);
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unknown register 0x%02" HWADDR_PRIx "\n", __func__, addr);
        return 0;
    }
}

static void ipod_touch_wdt_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchWDTState *s = opaque;

    switch (addr) {
    case WDT_CTRL: {
        bool was_enabled = s->ctrl & WDT_CTRL_ENABLE;
        s->ctrl = val & ~WDT_CTRL_CLR_MASK;
        if (!was_enabled || (val & WDT_CTRL_CLR_MASK) == WDT_CTRL_CLR) {
            s->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        qemu_irq_lower(s->irq);
        ipod_touch_wdt_update_timer(s);
        break;
    }
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unknown register 0x%02" HWADDR_PRIx " (value 0x%08" PRIx64 ")\n",
                      __func__, addr, val);
        break;
    }
}

static const MemoryRegionOps ipod_touch_wdt_ops = {
    .read = ipod_touch_wdt_read,
    .write = ipod_touch_wdt_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void ipod_touch_wdt_reset(DeviceState *dev)
{
    IPodTouchWDTState *s = IPOD_TOUCH_WDT(dev);

    s->ctrl = 0;
    s->start_ns = 0;
    timer_del(s->timer);
    qemu_irq_lower(s->irq);
}

static void ipod_touch_wdt_init(Object *obj)
{
    IPodTouchWDTState *s = IPOD_TOUCH_WDT(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &ipod_touch_wdt_ops, s, TYPE_IPOD_TOUCH_WDT, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ipod_touch_wdt_expired, s);
}

static void ipod_touch_wdt_finalize(Object *obj)
{
    IPodTouchWDTState *s = IPOD_TOUCH_WDT(obj);
    timer_free(s->timer);
}

static Property ipod_touch_wdt_properties[] = {
    DEFINE_PROP_UINT32("clock-frequency", IPodTouchWDTState, clock_hz, 133000000),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_ipod_touch_wdt = {
    .name = TYPE_IPOD_TOUCH_WDT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(ctrl, IPodTouchWDTState),
        VMSTATE_INT64(start_ns, IPodTouchWDTState),
        VMSTATE_TIMER_PTR(timer, IPodTouchWDTState),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_wdt_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = ipod_touch_wdt_reset;
    dc->vmsd = &vmstate_ipod_touch_wdt;
    device_class_set_props(dc, ipod_touch_wdt_properties);
}

static const TypeInfo ipod_touch_wdt_type_info = {
    .name = TYPE_IPOD_TOUCH_WDT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchWDTState),
    .instance_init = ipod_touch_wdt_init,
    .instance_finalize = ipod_touch_wdt_finalize,
    .class_init = ipod_touch_wdt_class_init,
};

static void ipod_touch_wdt_register_types(void)
{
    type_register_static(&ipod_touch_wdt_type_info);
}

type_init(ipod_touch_wdt_register_types)
