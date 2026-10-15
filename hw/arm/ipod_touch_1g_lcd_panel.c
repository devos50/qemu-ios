/*
 * The LCD panel of the iPod touch (1st generation), on SPI1, which iBoot calls a Merlot panel.
 *
 * Commands are framed by the chip select, a GPIO. The first byte names a register, with bit 7 set to read it; a write
 * gives the value in the next byte, a read returns it there. Some registers are commands without data (0x11 leaves
 * sleep, 0x29 turns the display on). iBoot reads the panel's ID from 0x5a-0x5c and waits for bit 0 of 0x15 after
 * each step of the power-on sequence; the rest is kept as written. The ID is the one the ipod_touch_1g branch of
 * qemu-ios answered with.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/arm/ipod_touch_1g_lcd_panel.h"
#include "migration/vmstate.h"

#define PANEL_REG_STATUS    0x15
#define PANEL_STATUS_READY  0x01
#define PANEL_REG_ID        0x5a

static const uint8_t panel_id[3] = { 0x71, 0xc2, 0x00 };

static uint8_t it1g_panel_read(IPodTouch1GLCDPanelState *s, uint8_t addr)
{
    if (addr >= PANEL_REG_ID && addr < PANEL_REG_ID + sizeof(panel_id)) {
        return panel_id[addr - PANEL_REG_ID];
    }
    if (addr == PANEL_REG_STATUS) {
        return s->regs[addr] | PANEL_STATUS_READY;
    }
    return s->regs[addr];
}

static uint32_t it1g_panel_transfer(SSIPeripheral *dev, uint32_t value)
{
    IPodTouch1GLCDPanelState *s = IPOD_TOUCH_1G_LCD_PANEL(dev);
    uint32_t pos = s->pos++;

    if (pos == 0) {
        s->addr = value & 0x7f;
        s->read = value & 0x80;
        return 0;
    }
    if (pos > 1) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: byte %u of a command to register 0x%02x\n", __func__, pos, s->addr);
        return 0;
    }
    if (s->read) {
        return it1g_panel_read(s, s->addr);
    }
    s->regs[s->addr] = value;
    return 0;
}

static int it1g_panel_set_cs(SSIPeripheral *dev, bool level)
{
    IPodTouch1GLCDPanelState *s = IPOD_TOUCH_1G_LCD_PANEL(dev);

    s->pos = 0;
    return 0;
}

static void it1g_panel_realize(SSIPeripheral *dev, Error **errp)
{
}

static void it1g_panel_reset(DeviceState *dev)
{
    IPodTouch1GLCDPanelState *s = IPOD_TOUCH_1G_LCD_PANEL(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->addr = 0;
    s->read = false;
    s->pos = 0;
}

static const VMStateDescription vmstate_it1g_panel = {
    .name = TYPE_IPOD_TOUCH_1G_LCD_PANEL,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(parent_obj, IPodTouch1GLCDPanelState),
        VMSTATE_UINT8_ARRAY(regs, IPodTouch1GLCDPanelState, IT1G_PANEL_REGS),
        VMSTATE_UINT8(addr, IPodTouch1GLCDPanelState),
        VMSTATE_BOOL(read, IPodTouch1GLCDPanelState),
        VMSTATE_UINT32(pos, IPodTouch1GLCDPanelState),
        VMSTATE_END_OF_LIST()
    }
};

static void it1g_panel_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);

    dc->vmsd = &vmstate_it1g_panel;
    device_class_set_legacy_reset(dc, it1g_panel_reset);
    k->realize = it1g_panel_realize;
    k->transfer = it1g_panel_transfer;
    k->set_cs = it1g_panel_set_cs;
    k->cs_polarity = SSI_CS_LOW;
}

static const TypeInfo it1g_panel_info = {
    .name = TYPE_IPOD_TOUCH_1G_LCD_PANEL,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(IPodTouch1GLCDPanelState),
    .class_init = it1g_panel_class_init,
};

static void it1g_panel_register_types(void)
{
    type_register_static(&it1g_panel_info);
}

type_init(it1g_panel_register_types)
