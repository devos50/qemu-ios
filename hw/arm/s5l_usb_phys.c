#include "hw/arm/s5l_usb_phys.h"
#include "migration/vmstate.h"

static uint64_t s5l_usb_phys_read(void *opaque, hwaddr addr, unsigned size)
{
    S5LUSBPhysState *s = (S5LUSBPhysState *) opaque;

    switch(addr)
    {
    case REG_OPHYPWR:
        return s->usb_ophypwr;
    case REG_OPHYCLK:
        return s->usb_ophyclk;
    case REG_ORSTCON:
        return s->usb_orstcon;
    case REG_UNKNOWN1:
        return s->usb_unknown1;
    case REG_OPHYTUNE:
        return s->usb_ophytune;
    case REG_CABLE:
        return s->cable_connected;
    default:
        //return 0x0;
        printf("%s: read invalid location 0x%08x\n", __func__, addr);
	return 0x0;
    }
}

static void s5l_usb_phys_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    S5LUSBPhysState *s = (S5LUSBPhysState *) opaque;

    switch(addr)
    {
    case REG_OPHYPWR:
        s->usb_ophypwr = val;
        return;
    case REG_OPHYCLK:
        s->usb_ophyclk = val;
        return;
    case REG_ORSTCON:
        s->usb_orstcon = val;
        return;
    case REG_UNKNOWN1:
        s->usb_unknown1 = val;
        return;
    case REG_OPHYTUNE:
        s->usb_ophytune = val;
        return;

    default:
        printf("%s: write invalid location 0x%08x.\n", __func__, addr);
        return;
    }
}

static const MemoryRegionOps s5l_usb_phys_ops = {
    .read = s5l_usb_phys_read,
    .write = s5l_usb_phys_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void s5l_usb_phys_init(Object *obj)
{
    S5LUSBPhysState *s = S5L_USB_PHYS(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &s5l_usb_phys_ops, s, TYPE_S5L_USB_PHYS, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_s5l_usb_phys = {
    .name = TYPE_S5L_USB_PHYS,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(usb_ophypwr, S5LUSBPhysState),
        VMSTATE_UINT32(usb_ophyclk, S5LUSBPhysState),
        VMSTATE_UINT32(usb_orstcon, S5LUSBPhysState),
        VMSTATE_UINT32(usb_unknown1, S5LUSBPhysState),
        VMSTATE_UINT32(usb_ophytune, S5LUSBPhysState),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l_usb_phys_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_s5l_usb_phys;
}

static const TypeInfo s5l_usb_phys_type_info = {
    .name = TYPE_S5L_USB_PHYS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5LUSBPhysState),
    .instance_init = s5l_usb_phys_init,
    .class_init = s5l_usb_phys_class_init,
};

static void s5l_usb_phys_register_types(void)
{
    type_register_static(&s5l_usb_phys_type_info);
}

type_init(s5l_usb_phys_register_types)
