/*
 * The iPod touch (1st generation), n45ap, with the S5L8900.
 *
 * Ported from the ipod_touch_1g branch of qemu-ios, which started a decrypted iBoot with the bootrom's routines
 * patched. This machine starts the firmware the way the bootrom does, from the NOR, so that it needs nothing from a
 * real device. It is early: the blocks that aren't modelled yet are unimplemented devices (-d unimp logs them).
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "crypto/aes.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/misc/unimp.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/address-spaces.h"
#include "system/physmem.h"
#include "system/system.h"
#include "system/reset.h"
#include "target/arm/cpregs.h"
#include "ui/input.h"
#include "standard-headers/linux/input-event-codes.h"
#include "hw/arm/ipod_touch_1g.h"
#include "hw/arm/s5l8900_bootrom.h"
#include "hw/arm/s5l8900_clock.h"
#include "hw/arm/s5l8900_lcd.h"
#include "hw/arm/s5l8900_nand.h"
#include "hw/arm/s5l8900_adm.h"
#include "hw/arm/s5l8900_tvout.h"
#include "hw/dma/pl080.h"
#include "hw/arm/s5l_chipid.h"
#include "hw/arm/s5l_sha1.h"
#include "hw/arm/s5l_mbx.h"
#include "hw/arm/ipod_touch_aes.h"
#include "hw/arm/ipod_touch_1g_lcd_panel.h"
#include "hw/arm/s5l_spi.h"
#include "hw/i2c/ipod_touch_i2c.h"
#include "hw/i2c/i2c.h"
#include "hw/block/flash.h"
#include "system/block-backend-global-state.h"
#include "system/block-backend-io.h"

#define IMG2_SUPERBLOCK_MAGIC   0x494d4732  // 'IMG2', the NOR's image list
#define IMG2_IMAGE_MAGIC        0x496d6732  // 'Img2', one image
#define IMG2_HEADER_SIZE        0x400
#define IMG2_TYPE_LLB           0x6c6c627a  // 'llbz'
#define IT1G_SUPERBLOCK_OFFSET  0x8400      // after the SysCfg at 0x4000

#define IMG2_FLAG_ENCRYPTED     0x200       // the data is encrypted with the device's image key

/*
 * The image key of an installed Img2 image: these 16 bytes encrypted (AES-128-CBC) with the UID key and IV, as LLB
 * derives it. A restore encrypts the data of the images it writes to the NOR with it (and signs them).
 */
static const uint8_t img2_key_seed[16] = {
    0xcd, 0xf3, 0x45, 0xb3, 0x12, 0xe7, 0x48, 0x85, 0x8b, 0xbe, 0x21, 0x47, 0xf0, 0xe5, 0x80, 0x88,
};
static const uint8_t img2_key_iv[16] = {
    0x41, 0x70, 0x5d, 0x11, 0x6f, 0x98, 0x4b, 0x82, 0x9c, 0x6c, 0x99, 0xbb, 0xa5, 0xf1, 0x78, 0x69,
};

static void allocate_ram(MemoryRegion *top, const char *name, hwaddr addr, uint64_t size)
{
    MemoryRegion *sec = g_new(MemoryRegion, 1);
    memory_region_init_ram(sec, NULL, name, size, &error_fatal);
    memory_region_add_subregion(top, addr, sec);
}

static inline qemu_irq s5l8900_get_irq(IPodTouch1GMachineState *s, int n)
{
    return s->irq[n / S5L8900_VIC_SIZE][n % S5L8900_VIC_SIZE];
}

static uint64_t it1g_periph_remap_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    return IPOD_TOUCH_1G_MACHINE(qdev_get_machine())->periph_port_remap;
}

static void it1g_periph_remap_write(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t value)
{
    IPOD_TOUCH_1G_MACHINE(qdev_get_machine())->periph_port_remap = value;
}

/*
 * The cache operations of the bootloaders, which TCG has no use for, and the ARM1176 peripheral port memory remap
 * register, which the CPU model lacks.
 */
static const ARMCPRegInfo it1g_cp_reginfo_tcg[] = {
    { .cp = 15, .name = "INVALIDATE_DCACHE", .crn = 7, .crm = 6, .opc1 = 0, .opc2 = 0,
      .access = PL1_W, .state = ARM_CP_STATE_AA32, .type = ARM_CP_NOP | ARM_CP_OVERRIDE },
    { .cp = 15, .name = "CLEAN_INVALIDATE_DCACHE", .crn = 7, .crm = 14, .opc1 = 0, .opc2 = 0,
      .access = PL1_W, .state = ARM_CP_STATE_AA32, .type = ARM_CP_NOP | ARM_CP_OVERRIDE },
    { .cp = 15, .name = "CLEAN_DCACHE", .crn = 7, .crm = 10, .opc1 = 0, .opc2 = 0,
      .access = PL1_W, .state = ARM_CP_STATE_AA32, .type = ARM_CP_NOP | ARM_CP_OVERRIDE },
    { .cp = 15, .name = "PERIPH_PORT_REMAP", .crn = 15, .crm = 2, .opc1 = 0, .opc2 = 4,
      .access = PL1_RW, .state = ARM_CP_STATE_AA32, .type = ARM_CP_NO_RAW | ARM_CP_OVERRIDE,
      .readfn = it1g_periph_remap_read, .writefn = it1g_periph_remap_write },
};

/* Decrypts the data of an 8900 container with key 0x837; returns its payload. */
static uint8_t *s5l8900_unwrap_8900(const uint8_t *file, size_t size, size_t *payload_size, Error **errp)
{
    uint32_t len = ldl_le_p(file + 0xc);
    if (size < S5L8900_8900_HEADER || len > size - S5L8900_8900_HEADER) {
        error_setg(errp, "a truncated 8900 container");
        return NULL;
    }
    uint8_t *out = g_memdup2(file + S5L8900_8900_HEADER, len);
    if (file[7] == S5L8900_8900_ENCRYPTED) {
        static const uint8_t zero_iv[AES_BLOCK_SIZE];
        s5l8900_aes_cbc(s5l8900_key_0x837, zero_iv, out, len, false);
    }
    *payload_size = len;
    return out;
}

/*
 * Finds an image in the NOR's image list. The superblock: magic, granule, image offset, boot block size (all in
 * granules from the start of the NOR); every image's header holds its length in granules at 0x18.
 */
static bool it1g_find_nor_image(const uint8_t *nor, size_t nor_size, uint32_t type,
                                const uint8_t **image, size_t *image_size)
{
    const uint8_t *sb = nor + IT1G_SUPERBLOCK_OFFSET;
    if (nor_size < IT1G_SUPERBLOCK_OFFSET + 0x10 || ldl_le_p(sb) != IMG2_SUPERBLOCK_MAGIC) {
        return false;
    }
    uint64_t granule = ldl_le_p(sb + 0x4);
    uint64_t offset = ((uint64_t)ldl_le_p(sb + 0x8) + ldl_le_p(sb + 0xc)) * granule;

    while (granule && offset + IMG2_HEADER_SIZE <= nor_size && ldl_le_p(nor + offset) == IMG2_IMAGE_MAGIC) {
        uint64_t len = (uint64_t)ldl_le_p(nor + offset + 0x18) * granule;
        if (len < IMG2_HEADER_SIZE || offset + len > nor_size) {
            return false;
        }
        if (ldl_le_p(nor + offset + 0x4) == type) {
            *image = nor + offset;
            *image_size = len;
            return true;
        }
        offset += len;
    }
    return false;
}

/*
 * Without a bootrom the machine does its job: it puts the data of the first boot stage (the boot-image, an Img2 image
 * or its 8900 container, or LLB from the NOR, decrypted if it was installed encrypted) at the start of SRAM and starts
 * it with r0 = 0.
 */
static bool it1g_load_first_stage(IPodTouch1GMachineState *nms, Error **errp)
{
    g_autofree uint8_t *file = NULL;
    g_autofree uint8_t *unwrapped = NULL;
    const uint8_t *image;
    size_t size;

    if (nms->boot_image_path) {
        gsize fsize;
        if (!g_file_get_contents(nms->boot_image_path, (char **)&file, &fsize, NULL)) {
            error_setg(errp, "boot-image: unable to read %s", nms->boot_image_path);
            return false;
        }
        image = file;
        size = fsize;
        if (size >= 8 && ldl_le_p(image) == S5L8900_8900_MAGIC) {
            unwrapped = s5l8900_unwrap_8900(image, size, &size, errp);
            if (!unwrapped) {
                return false;
            }
            image = unwrapped;
        }
    } else if (!it1g_find_nor_image(memory_region_get_ram_ptr(sysbus_mmio_get_region(SYS_BUS_DEVICE(nms->nor), 0)),
                                    IT1G_NOR_SIZE, IMG2_TYPE_LLB,
                                    &image, &size)) {
        error_setg(errp, "the NOR has no LLB image to boot");
        return false;
    }

    if (size < IMG2_HEADER_SIZE || ldl_le_p(image) != IMG2_IMAGE_MAGIC) {
        error_setg(errp, "%s isn't an Img2 image", nms->boot_image_path ? "boot-image" : "LLB in the NOR");
        return false;
    }
    uint32_t len = ldl_le_p(image + 0x10);
    if (len > size - IMG2_HEADER_SIZE || len > IT1G_SRAM_SIZE) {
        error_setg(errp, "the first boot stage is %u bytes, more than its image or SRAM holds", len);
        return false;
    }
    g_autofree uint8_t *data = g_memdup2(image + IMG2_HEADER_SIZE, len);
    if (ldl_le_p(image + 0x1c) & IMG2_FLAG_ENCRYPTED) {
        // as the bootrom does, without checking the signatures
        static const uint8_t zero_iv[AES_BLOCK_SIZE];
        uint8_t key[16];
        memcpy(key, img2_key_seed, sizeof(key));
        s5l8900_aes_cbc(key_uid, img2_key_iv, key, sizeof(key), true);
        s5l8900_aes_cbc(key, zero_iv, data, len, false);
    }
    physical_memory_write(IT1G_SRAM_MEM_BASE, data, len);
    return true;
}

static void it1g_cpu_reset(void *opaque)
{
    IPodTouch1GMachineState *nms = IPOD_TOUCH_1G_MACHINE(opaque);
    CPUState *cs = CPU(nms->cpu);
    Error *err = NULL;

    cpu_reset(cs);
    if (!it1g_load_first_stage(nms, &err)) {
        error_report_err(err);
        exit(1);
    }
    nms->cpu->env.regs[0] = 0;
    cpu_set_pc(cs, IT1G_SRAM_MEM_BASE);
}

/*
 * The power (hold) and home (menu) buttons, on the P and H keys as on the 2G. A press or release changes the pad and
 * interrupts on both edges, as the kernel's buttons driver expects.
 */
static void it1g_key_event(DeviceState *dev, QemuConsole *src, QemuInputEvent *evt)
{
    IPodTouch1GMachineState *nms = IPOD_TOUCH_1G_MACHINE(qdev_get_machine());
    uint32_t gpio;
    int irq;

    switch (evt->key.key) {
    case KEY_P:
        gpio = IT1G_GPIO_BUTTON_POWER;
        irq = IT1G_GPIO_BUTTON_POWER_IRQ;
        break;
    case KEY_H:
        gpio = IT1G_GPIO_BUTTON_HOME;
        irq = IT1G_GPIO_BUTTON_HOME_IRQ;
        break;
    default:
        return;
    }
    if (evt->key.down == gpio_is_on(nms->gpio_state->gpio_state, gpio)) {
        return;     // a key repeat
    }
    if (evt->key.down) {
        gpio_set_on(nms->gpio_state->gpio_state, gpio);
    } else {
        gpio_set_off(nms->gpio_state->gpio_state, gpio);
    }
    qemu_irq_pulse(qdev_get_gpio_in(nms->sysic, irq));
}

static const QemuInputHandler it1g_key_handler = {
    .name  = "iPod touch 1G buttons",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = it1g_key_event,
};

/*
 * The touchscreen: the left mouse button puts a finger on the Z2 multi-touch controller where the pointer is, as on
 * the 2G. The pointer takes absolute positions over the screen; the multi-touch Y axis points up.
 */
static void it1g_touch_event(DeviceState *dev, QemuConsole *src, QemuInputEvent *evt)
{
    IPodTouch1GMachineState *nms = IPOD_TOUCH_1G_MACHINE(qdev_get_machine());

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS:
        if (evt->abs.axis < INPUT_AXIS__MAX) {
            nms->touch_axis[evt->abs.axis] = evt->abs.value;
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        if (evt->btn.button == INPUT_BUTTON_LEFT) {
            nms->touch_button = evt->btn.down;
        }
        break;
    default:
        break;
    }
}

static void it1g_touch_sync(DeviceState *dev)
{
    IPodTouch1GMachineState *nms = IPOD_TOUCH_1G_MACHINE(qdev_get_machine());
    IPodTouchMultitouchState *mt = nms->mt;

    mt->touch_x = (float)nms->touch_axis[INPUT_AXIS_X] / INPUT_EVENT_ABS_MAX;
    mt->touch_y = 1 - (float)nms->touch_axis[INPUT_AXIS_Y] / INPUT_EVENT_ABS_MAX;
    if (nms->touch_button && !mt->touch_down) {
        ipod_touch_multitouch_on_touch(mt);
    } else if (!nms->touch_button && mt->touch_down) {
        ipod_touch_multitouch_on_release(mt);
    }
}

static const QemuInputHandler it1g_touch_handler = {
    .name  = "iPod touch 1G touchscreen",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = it1g_touch_event,
    .sync  = it1g_touch_sync,
};

static void it1g_memory_setup(IPodTouch1GMachineState *nms, MemoryRegion *sysmem)
{
    allocate_ram(sysmem, "ram", IT1G_RAM_MEM_BASE, IT1G_RAM_SIZE);
    // whether this is memory of its own or another window on the DRAM isn't known; its own is the safe guess
    allocate_ram(sysmem, "iboot-ram", IT1G_IBOOT_RAM_BASE, IT1G_IBOOT_RAM_SIZE);
    allocate_ram(sysmem, "sram", IT1G_SRAM_MEM_BASE, IT1G_SRAM_SIZE);

    /*
     * The NOR: 1 MB of SST CFI flash on a 16-bit bus, which LLB identifies by its JEDEC IDs. Read-only for now: the
     * boot stages only read it.
     */
    if (!nms->nor_path) {
        error_report("the iPod touch 1G needs a NOR image (nor=)");
        exit(1);
    }
    Error *err = NULL;
    BlockBackend *blk = blk_new_open(nms->nor_path, NULL, NULL, 0, &err);
    if (!blk) {
        error_reportf_err(err, "nor: ");
        exit(1);
    }
    if (blk_getlength(blk) != IT1G_NOR_SIZE) {
        error_report("nor: %s isn't a %d byte NOR image", nms->nor_path, IT1G_NOR_SIZE);
        exit(1);
    }
    nms->nor = pflash_cfi02_register(IT1G_NOR_MEM_BASE, "nor", IT1G_NOR_SIZE, blk, 4096, 1, 2,
                                     0x00bf, 0x273f, 0, 0, 0x555, 0x2aa, 0);

    // the blocks without a model yet
    static const struct { const char *name; hwaddr base; uint64_t size; } unimp_regions[] = {
        { "clock0",     IT1G_CLOCK0_MEM_BASE,   0x1000 },
        { "usb-otg",    IT1G_USBOTG_MEM_BASE,   0x10000 },
        { "sdio",       IT1G_SDIO_MEM_BASE,     0x1000 },
        { "edgeic",     IT1G_EDGEIC_MEM_BASE,   0x1000 },
        { "mpvd",       IT1G_MPVD_MEM_BASE,     0x70000 },
        { "h264bpd",    IT1G_H264BPD_MEM_BASE,  0x1000 },
        { "usb-phys",   IT1G_USBPHYS_MEM_BASE,  0x1000 },
        { "iis2",       IT1G_IIS2_MEM_BASE,     0x1000 },
        { "iis1",       IT1G_IIS1_MEM_BASE,     0x1000 },
        { "iis0",       IT1G_IIS0_MEM_BASE,     0x1000 },
        { "watchdog",   IT1G_WATCHDOG_MEM_BASE, 0x1000 },
    };
    for (int i = 0; i < ARRAY_SIZE(unimp_regions); i++) {
        create_unimplemented_device(unimp_regions[i].name, unimp_regions[i].base, unimp_regions[i].size);
    }
}

static void it1g_machine_init(MachineState *machine)
{
    IPodTouch1GMachineState *nms = IPOD_TOUCH_1G_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *dev;
    SysBusDevice *busdev;

    if (nms->bootrom_path) {
        error_report("the iPod touch 1G can't boot through a bootrom yet; leave bootrom= out");
        exit(1);
    }

    Object *cpuobj = object_new(machine->cpu_type);
    nms->cpu = ARM_CPU(cpuobj);
    object_property_set_link(cpuobj, "memory", OBJECT(sysmem), &error_abort);
    object_property_set_bool(cpuobj, "has_el3", false, NULL);
    object_property_set_bool(cpuobj, "has_el2", false, NULL);
    object_property_set_bool(cpuobj, "realized", true, &error_fatal);
    define_arm_cp_regs(nms->cpu, it1g_cp_reginfo_tcg);
    object_unref(cpuobj);

    it1g_memory_setup(nms, sysmem);

    // the two VICs, chained
    nms->irq = g_malloc0(sizeof(qemu_irq *) * 2);
    dev = pl192_manual_init("vic0", qdev_get_gpio_in(DEVICE(nms->cpu), ARM_CPU_IRQ),
                            qdev_get_gpio_in(DEVICE(nms->cpu), ARM_CPU_FIQ), NULL);
    nms->vic0 = PL192(dev);
    memory_region_add_subregion(sysmem, IT1G_VIC0_MEM_BASE, &nms->vic0->iomem);
    nms->irq[0] = g_malloc0(sizeof(qemu_irq) * S5L8900_VIC_SIZE);
    for (int i = 0; i < S5L8900_VIC_SIZE; i++) {
        nms->irq[0][i] = qdev_get_gpio_in(dev, i);
    }
    dev = pl192_manual_init("vic1", NULL);
    nms->vic1 = PL192(dev);
    memory_region_add_subregion(sysmem, IT1G_VIC1_MEM_BASE, &nms->vic1->iomem);
    nms->irq[1] = g_malloc0(sizeof(qemu_irq) * S5L8900_VIC_SIZE);
    for (int i = 0; i < S5L8900_VIC_SIZE; i++) {
        nms->irq[1][i] = qdev_get_gpio_in(dev, i);
    }
    nms->vic1->daisy = nms->vic0;
    nms->vic0->daisy_callback = nms->vic1;

    sysbus_create_simple(TYPE_S5L8900_CLOCK, IT1G_CLOCK1_MEM_BASE, NULL);
    DeviceState *lcd = sysbus_create_simple(TYPE_S5L8900_LCD, IT1G_DISPLAY_MEM_BASE,
                                            s5l8900_get_irq(nms, S5L8900_LCD_IRQ));

    // the TV out: the SDO, the mixer and the third block
    dev = qdev_new(TYPE_S5L8900_TVOUT);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(busdev, &error_fatal);
    sysbus_mmio_map(busdev, S5L8900_TVOUT_SDO, IT1G_TVOUT3_MEM_BASE);
    sysbus_mmio_map(busdev, S5L8900_TVOUT_MIXER, IT1G_TVOUT2_MEM_BASE);
    sysbus_mmio_map(busdev, S5L8900_TVOUT_VP, IT1G_TVOUT1_MEM_BASE);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8900_TVOUT_SDO_IRQ));
    sysbus_connect_irq(busdev, 1, s5l8900_get_irq(nms, S5L8900_TVOUT_MIXER_IRQ));

    // the same MBX Lite as the S5L8720's, without the wrapper
    dev = qdev_new(TYPE_S5L_MBX);
    busdev = SYS_BUS_DEVICE(dev);
    memory_region_add_subregion(sysmem, IT1G_MBX_MEM_BASE, &S5L_MBX(dev)->iomem1);
    sysbus_realize_and_unref(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8900_MBX_IRQ));

    // in place of the bootrom, what iBoot calls in it (the machine itself does the bootrom's loading of LLB)
    sysbus_create_simple(TYPE_S5L8900_BOOTROM, IT1G_VROM_MEM_BASE, NULL);

    dev = qdev_new(TYPE_S5L_CHIPID);
    qdev_prop_set_uint32(dev, "chip", 0x8900);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, IT1G_CHIPID_MEM_BASE);

    // the same SHA-1 and AES engines as the S5L8720's; the AES engine differs in two details (see its properties)
    sysbus_create_simple(TYPE_S5L_SHA1, IT1G_SHA1_MEM_BASE, NULL);
    dev = qdev_new(TYPE_IPOD_TOUCH_AES);
    qdev_prop_set_uint32(dev, "dma-mask", 0x7fffffff);
    qdev_prop_set_bit(dev, "big-endian-words", true);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(sysmem, IT1G_AES_MEM_BASE, &IPOD_TOUCH_AES(dev)->iomem);

    dev = qdev_new(TYPE_S5L_TIMER);
    qdev_prop_set_uint32(dev, "irqstat-offset", 0x10000);
    qdev_prop_set_uint32(dev, "irqlatch-offset", 0xf8);
    qdev_prop_set_uint32(dev, "timers", 7);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(busdev, &error_fatal);
    sysbus_mmio_map(busdev, 0, IT1G_TIMER1_MEM_BASE);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8900_TIMER1_IRQ));
    nms->timer1 = S5L_TIMER(dev);

    dev = qdev_new(TYPE_S5L_SYSIC);
    nms->sysic = dev;
    qdev_prop_set_bit(dev, "power-off-bits", true);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(busdev, &error_fatal);
    sysbus_mmio_map(busdev, 0, IT1G_SYSIC_MEM_BASE);
    static const int gpio_irqs[] = {
        S5L8900_GPIO_G0_IRQ, S5L8900_GPIO_G1_IRQ, S5L8900_GPIO_G2_IRQ, S5L8900_GPIO_G3_IRQ, S5L8900_GPIO_G4_IRQ,
        S5L8900_GPIO_G5_IRQ, S5L8900_GPIO_G6_IRQ,
    };
    for (int grp = 0; grp < ARRAY_SIZE(gpio_irqs); grp++) {
        sysbus_connect_irq(busdev, grp, s5l8900_get_irq(nms, gpio_irqs[grp]));
    }

    dev = qdev_new(TYPE_S5L_GPIO);
    nms->gpio_state = S5L_GPIO(dev);
    qdev_prop_set_uint32(dev, "fsel-offset", IT1G_GPIO_FSEL);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(sysmem, IT1G_GPIO_MEM_BASE, &nms->gpio_state->iomem);

    /*
     * The SPI controllers: the same as the S5L8720's but with 8-byte FIFOs, the word size and the FIFO levels at
     * other bits, and no TX count (see iBoot's driver). The LCD panel is on SPI1, the multi-touch controller on SPI2;
     * iBoot also writes to something on SPI0.
     */
    static const struct { hwaddr base; int irq; } spis[] = {
        { IT1G_SPI0_MEM_BASE, S5L8900_SPI0_IRQ },
        { IT1G_SPI1_MEM_BASE, S5L8900_SPI1_IRQ },
        { IT1G_SPI2_MEM_BASE, S5L8900_SPI2_IRQ },
    };
    for (int i = 0; i < ARRAY_SIZE(spis); i++) {
        set_spi_base(i);
        dev = qdev_new(TYPE_S5L_SPI);
        qdev_prop_set_uint8(dev, "cfg-word-size-shift", 13);
        qdev_prop_set_uint8(dev, "status-tx-level-shift", 4);
        qdev_prop_set_uint8(dev, "status-rx-level-shift", 8);
        qdev_prop_set_uint8(dev, "status-level-width", 4);
        qdev_prop_set_uint32(dev, "rx-fifo-depth", 8);
        qdev_prop_set_bit(dev, "tx-count", false);
        busdev = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(busdev, &error_fatal);
        sysbus_mmio_map(busdev, 0, spis[i].base);
        sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, spis[i].irq));
        nms->spi[i] = S5L_SPI(dev);
    }
    dev = ssi_create_peripheral(nms->spi[1]->spi, TYPE_IPOD_TOUCH_1G_LCD_PANEL);
    qdev_connect_gpio_out(DEVICE(nms->gpio_state), GPIO_OUT_INDEX(IT1G_GPIO_LCD_CS),
                          qdev_get_gpio_in_named(dev, SSI_GPIO_CS, 0));
    /*
     * The same Z2 multi-touch controller as the 2G's (family 81, 15 rows by 10 columns), which the 1.1.5 driver
     * (AppleMultitouchZ2SPI) bootloads and reads frames from the same way.
     */
    dev = ssi_create_peripheral(nms->spi[2]->spi, TYPE_IPOD_TOUCH_MULTITOUCH);
    nms->mt = IPOD_TOUCH_MULTITOUCH(dev);
    qdev_connect_gpio_out_named(dev, "irq", 0, qdev_get_gpio_in(nms->sysic, IT1G_GPIO_MULTITOUCH_IRQ));
    qdev_connect_gpio_out(DEVICE(nms->gpio_state), GPIO_OUT_INDEX(IT1G_GPIO_MULTITOUCH_CS),
                          qdev_get_gpio_in_named(dev, SSI_GPIO_CS, 0));
    qdev_connect_gpio_out(DEVICE(nms->gpio_state), GPIO_OUT_INDEX(IT1G_GPIO_MULTITOUCH_RESET),
                          qdev_get_gpio_in_named(dev, "reset", 0));

    // the DMA controllers, ARM PL080s; the NAND controller is peripheral 8 of DMAC0
    static const struct { hwaddr base; int irq; } dmacs[] = {
        { IT1G_DMAC0_MEM_BASE, S5L8900_DMAC0_IRQ }, { IT1G_DMAC1_MEM_BASE, S5L8900_DMAC1_IRQ },
    };
    for (int i = 0; i < ARRAY_SIZE(dmacs); i++) {
        dev = qdev_new(TYPE_PL080);
        object_property_set_link(OBJECT(dev), "downstream", OBJECT(sysmem), &error_fatal);
        busdev = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(busdev, &error_fatal);
        sysbus_mmio_map(busdev, 0, dmacs[i].base);
        sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, dmacs[i].irq));
    }

    // the NAND and its ECC engine
    if (nms->nand_epoch) {
        error_report("nand-epoch: the iPod touch 1G's NAND driver has no security epoch; leave it out");
        exit(1);
    }
    if (!nms->nand_disk != !nms->nand_overlay) {
        error_report("nand-disk and nand-overlay go together");
        exit(1);
    }
    dev = qdev_new(TYPE_S5L8900_NAND);
    qdev_prop_set_string(dev, "disk", nms->nand_disk);
    qdev_prop_set_string(dev, "overlay", nms->nand_overlay);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(busdev, &error_fatal);
    sysbus_mmio_map(busdev, 0, IT1G_NAND_MEM_BASE);
    sysbus_create_simple(TYPE_S5L8900_NAND_ECC, IT1G_NAND_ECC_MEM_BASE, s5l8900_get_irq(nms, S5L8900_NAND_ECC_IRQ));

    // the ADM, which drives the NAND for the kernel
    DeviceState *nand = dev;
    dev = qdev_new(TYPE_S5L8900_ADM);
    object_property_set_link(OBJECT(dev), "nand", OBJECT(nand), &error_fatal);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(busdev, &error_fatal);
    sysbus_mmio_map(busdev, 0, IT1G_ADM_MEM_BASE);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8900_ADM_IRQ));
    qdev_connect_gpio_out_named(nand, "stream-done", 0, qdev_get_gpio_in_named(dev, "stream-done", 0));

    // Samsung UARTs, like the Exynos one
    for (int i = 0; i < IT1G_UART_COUNT; i++) {
        dev = qdev_new("exynos4210.uart");
        qdev_prop_set_chr(dev, "chardev", serial_hd(i));
        qdev_prop_set_uint32(dev, "channel", i);
        qdev_prop_set_uint32(dev, "rx-size", 256);
        qdev_prop_set_uint32(dev, "tx-size", 256);
        busdev = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(busdev, &error_fatal);
        sysbus_mmio_map(busdev, 0, IT1G_UART0_MEM_BASE + i * IT1G_UART_STRIDE);
        sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8900_UART0_IRQ + i));
    }

    // the I2C buses: the accelerometer and the light sensor on I2C0, the PMU on I2C1
    static const struct { hwaddr base; int irq; } i2cs[] = {
        { IT1G_I2C0_MEM_BASE, S5L8900_I2C0_IRQ }, { IT1G_I2C1_MEM_BASE, S5L8900_I2C1_IRQ },
    };
    IPodTouchI2CState *i2c[ARRAY_SIZE(i2cs)];
    for (int i = 0; i < ARRAY_SIZE(i2cs); i++) {
        dev = qdev_new(TYPE_IPOD_TOUCH_I2C);
        i2c[i] = IPOD_TOUCH_I2C(dev);
        i2c[i]->base = i;
        busdev = SYS_BUS_DEVICE(dev);
        memory_region_add_subregion(sysmem, i2cs[i].base, &i2c[i]->iomem);
        sysbus_realize(busdev, &error_fatal);
        sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, i2cs[i].irq));
    }
    i2c_slave_create_simple(i2c[0]->bus, "lis302dl", 0x1D);
    // the ambient light sensor, whose reading auto-brightness follows (dark without it)
    dev = DEVICE(i2c_slave_new("isl29003dl", 0x44));
    object_property_add_child(OBJECT(machine), "lightsensor", OBJECT(dev));
    i2c_slave_realize_and_unref(I2C_SLAVE(dev), i2c[0]->bus, &error_fatal);
    qdev_connect_gpio_out(dev, 0, qdev_get_gpio_in(nms->sysic, IT1G_GPIO_LIGHTSENSOR_IRQ));
    /*
     * The PMU is a PCF50635, with its backlight in the LED registers. ApplePCF50635PMUBacklight maps its brightness
     * (0-127) to LEDOUT through a table that ends at 0x2e, which is full brightness here.
     */
    dev = DEVICE(i2c_slave_new("pcf50633", 0x73));
    qdev_prop_set_uint8(dev, "backlight-level-reg", 0x28);
    qdev_prop_set_uint8(dev, "backlight-ctrl-reg", 0x29);
    qdev_prop_set_uint8(dev, "backlight-level-max", 0x2e);
    i2c_slave_realize_and_unref(I2C_SLAVE(dev), i2c[1]->bus, &error_fatal);
    qdev_connect_gpio_out_named(dev, "backlight", 0, qdev_get_gpio_in_named(lcd, "backlight", 0));

    qemu_register_reset(it1g_cpu_reset, nms);

    qemu_input_handler_activate(qemu_input_handler_register(nms->sysic, &it1g_key_handler));
    qemu_input_handler_activate(qemu_input_handler_register(DEVICE(nms->mt), &it1g_touch_handler));
}

static char *it1g_get_nor(Object *obj, Error **errp)
{
    return g_strdup(IPOD_TOUCH_1G_MACHINE(obj)->nor_path);
}

static void it1g_set_nor(Object *obj, const char *value, Error **errp)
{
    IPodTouch1GMachineState *nms = IPOD_TOUCH_1G_MACHINE(obj);
    g_free(nms->nor_path);
    nms->nor_path = g_strdup(value);
}

static char *it1g_get_boot_image(Object *obj, Error **errp)
{
    return g_strdup(IPOD_TOUCH_1G_MACHINE(obj)->boot_image_path);
}

static void it1g_set_boot_image(Object *obj, const char *value, Error **errp)
{
    IPodTouch1GMachineState *nms = IPOD_TOUCH_1G_MACHINE(obj);
    g_free(nms->boot_image_path);
    nms->boot_image_path = g_strdup(value);
}

static char *it1g_get_bootrom(Object *obj, Error **errp)
{
    return g_strdup(IPOD_TOUCH_1G_MACHINE(obj)->bootrom_path);
}

static void it1g_set_bootrom(Object *obj, const char *value, Error **errp)
{
    IPodTouch1GMachineState *nms = IPOD_TOUCH_1G_MACHINE(obj);
    g_free(nms->bootrom_path);
    nms->bootrom_path = g_strdup(value);
}

#define IT1G_STR_PROPERTY(field)                                                \
static char *it1g_get_##field(Object *obj, Error **errp)                        \
{                                                                               \
    return g_strdup(IPOD_TOUCH_1G_MACHINE(obj)->field);                         \
}                                                                               \
static void it1g_set_##field(Object *obj, const char *value, Error **errp)      \
{                                                                               \
    IPodTouch1GMachineState *nms = IPOD_TOUCH_1G_MACHINE(obj);                  \
    g_free(nms->field);                                                         \
    nms->field = g_strdup(value);                                               \
}

IT1G_STR_PROPERTY(nand_disk)
IT1G_STR_PROPERTY(nand_overlay)
IT1G_STR_PROPERTY(nand_epoch)

static void it1g_instance_init(Object *obj)
{
    object_property_add_str(obj, "nor", it1g_get_nor, it1g_set_nor);
    object_property_set_description(obj, "nor", "Path to the S5L8900 NOR image");
    object_property_add_str(obj, "boot-image", it1g_get_boot_image, it1g_set_boot_image);
    object_property_set_description(obj, "boot-image", "Img2 image (or its 8900 container) to start instead of LLB "
                                    "from the NOR");
    object_property_add_str(obj, "bootrom", it1g_get_bootrom, it1g_set_bootrom);
    object_property_set_description(obj, "bootrom", "Not supported on the iPod touch 1G yet");
    object_property_add_str(obj, "nand-disk", it1g_get_nand_disk, it1g_set_nand_disk);
    object_property_set_description(obj, "nand-disk", "Disk image presented as a freshly formatted NAND");
    object_property_add_str(obj, "nand-overlay", it1g_get_nand_overlay, it1g_set_nand_overlay);
    object_property_set_description(obj, "nand-overlay", "Directory for the NAND pages written on top of nand-disk");
    object_property_add_str(obj, "nand-epoch", it1g_get_nand_epoch, it1g_set_nand_epoch);
    object_property_set_description(obj, "nand-epoch", "Not supported on the iPod touch 1G (no NAND security epoch)");
}

static void it1g_machine_class_init(ObjectClass *klass, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(klass);
    mc->desc = "iPod touch (1st generation)";
    mc->init = it1g_machine_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm1176");
}

static const TypeInfo it1g_machine_info = {
    .name          = TYPE_IPOD_TOUCH_1G_MACHINE,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(IPodTouch1GMachineState),
    .class_size    = sizeof(IPodTouch1GMachineClass),
    .class_init    = it1g_machine_class_init,
    .instance_init = it1g_instance_init,
    .interfaces    = arm_machine_interfaces,
};

static void it1g_machine_types(void)
{
    type_register_static(&it1g_machine_info);
}

type_init(it1g_machine_types)
