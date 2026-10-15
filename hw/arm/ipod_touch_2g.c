#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "ui/input.h"
#include "system/address-spaces.h"
#include "hw/misc/unimp.h"
#include "hw/core/irq.h"
#include "system/system.h"
#include "system/reset.h"
#include "hw/core/platform-bus.h"
#include "hw/block/flash.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/exynos4210.h"
#include "hw/arm/ipod_touch_2g.h"
#include "hw/arm/ipod_touch_boot.h"
#include "hw/arm/ipod_touch_pcf50633_pmu.h"
#include "target/arm/cpregs.h"
#include "qemu/error-report.h"
#include "chardev/char.h"
#include "hw/core/qdev-properties-system.h"
#include "net/net.h"
#include "migration/vmstate.h"

static void allocate_ram(MemoryRegion *top, const char *name, uint32_t addr, uint32_t size)
{
    MemoryRegion *sec = g_new(MemoryRegion, 1);
    memory_region_init_ram(sec, NULL, name, size, &error_fatal);
    memory_region_add_subregion(top, addr, sec);
}

static uint64_t it2g_periph_remap_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    return IPOD_TOUCH_MACHINE(qdev_get_machine())->periph_port_remap;
}

static void it2g_periph_remap_write(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t value)
{
    IPOD_TOUCH_MACHINE(qdev_get_machine())->periph_port_remap = value;
}

/*
 * The cache operations of the bootloaders, and the ARM1176 peripheral port memory remap register, which the machine
 * keeps (and saves) since the register is not part of the CPU's state.
 */
static const ARMCPRegInfo it2g_cp_reginfo_tcg[] = {
    { .cp = 15, .name = "INVALIDATE_DCACHE", .crn = 7, .crm = 6, .opc1 = 0, .opc2 = 0,
      .access = PL1_W, .state = ARM_CP_STATE_AA32, .type = ARM_CP_NOP | ARM_CP_OVERRIDE },
    { .cp = 15, .name = "CLEAN_INVALIDATE_DCACHE", .crn = 7, .crm = 14, .opc1 = 0, .opc2 = 0,
      .access = PL1_W, .state = ARM_CP_STATE_AA32, .type = ARM_CP_NOP | ARM_CP_OVERRIDE },
    { .cp = 15, .name = "CLEAN_DCACHE", .crn = 7, .crm = 10, .opc1 = 0, .opc2 = 0,
      .access = PL1_W, .state = ARM_CP_STATE_AA32, .type = ARM_CP_NOP | ARM_CP_OVERRIDE },
    { .cp = 15, .name = "PERIPH_PORT_REMAP", .crn = 15, .crm = 2, .opc1 = 0, .opc2 = 4,
      .access = PL1_RW, .state = ARM_CP_STATE_AA32, .type = ARM_CP_NO_RAW | ARM_CP_OVERRIDE,
      .readfn = it2g_periph_remap_read, .writefn = it2g_periph_remap_write },
};

static void ipod_touch_cpu_setup(MachineState *machine, MemoryRegion **sysmem, ARMCPU **cpu, AddressSpace **nsas)
{
    Object *cpuobj = object_new(machine->cpu_type);
    *cpu = ARM_CPU(cpuobj);
    CPUState *cs = CPU(*cpu);

    *sysmem = get_system_memory();

    object_property_set_link(cpuobj, "memory", OBJECT(*sysmem), &error_abort);

    object_property_set_bool(cpuobj, "has_el3", false, NULL);

    object_property_set_bool(cpuobj, "has_el2", false, NULL);

    object_property_set_bool(cpuobj, "realized", true, &error_fatal);

    *nsas = cpu_get_address_space(cs, ARMASIdx_NS);

    define_arm_cp_regs(*cpu, it2g_cp_reginfo_tcg);

    object_unref(cpuobj);
}

/*
 * Without a bootrom the machine does its job: it loads the first boot stage (the boot-image, or LLB from the NOR as it
 * is now, which a restore may have rewritten) to SRAM and starts it like the bootrom does, with r0 = 0. The CPU's reset
 * state is what the bootrom leaves behind (SVC mode, interrupts masked, MMU and caches off).
 */
static bool ipod_touch_load_first_stage(IPodTouchMachineState *nms, Error **errp)
{
    const uint8_t *image = nms->boot_image;
    size_t size = nms->boot_image_size;

    if (!image && !ipod_touch_boot_find_nor_image(nms->nor_image->data, nms->nor_image->size, IMG3_TAG_LLB,
                                                  &image, &size)) {
        error_setg(errp, "the NOR has no LLB image to boot");
        return false;
    }
    if (!ipod_touch_boot_load_img3(image, size, errp)) {
        error_prepend(errp, nms->boot_image ? "boot-image: " : "LLB in the NOR: ");
        return false;
    }
    return true;
}

static void ipod_touch_cpu_reset(void *opaque)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE((MachineState *)opaque);
    ARMCPU *cpu = nms->cpu;
    CPUState *cs = CPU(cpu);
    Error *err = NULL;

    cpu_reset(cs);

    if (nms->bootrom_path[0]) {
        cpu_set_pc(cs, VROM_MEM_BASE);
    } else if (ipod_touch_load_first_stage(nms, &err)) {
        cpu->env.regs[0] = 0;
        cpu_set_pc(cs, IPOD_TOUCH_BOOT_ADDR);
    } else {
        // the NOR was valid at start-up, so the guest has broken it; the empty ROM area doesn't boot either
        error_report_err(err);
        cpu_set_pc(cs, VROM_MEM_BASE);
    }
}

static void ipod_touch_memory_setup(MachineState *machine, MemoryRegion *sysmem, AddressSpace *nsas)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(machine);

    allocate_ram(sysmem, "insecure_ram", INSECURE_RAM_MEM_BASE, 0x3000000);
    allocate_ram(sysmem, "secure_ram", SECURE_RAM_MEM_BASE, 0x4B04000);
    allocate_ram(sysmem, "iboot", IBOOT_MEM_BASE, 0x100000);
    allocate_ram(sysmem, "llb", 0x22000000, 0x100000);
    allocate_ram(sysmem, "sram1", SRAM1_MEM_BASE, 0x100000);
    allocate_ram(sysmem, "framebuffer", FRAMEBUFFER_MEM_BASE, 0x400000);
    allocate_ram(sysmem, "edgeic", EDGEIC_MEM_BASE, 0x1000);
    allocate_ram(sysmem, "swi", SWI_MEM_BASE, 0x1000);
    allocate_ram(sysmem, "h264", H264_MEM_BASE, 0x4000);

    /*
     * Back every register range from the device tree ("reg" of the arm-io
     * children) so a stray driver access is logged with -d unimp instead of
     * raising an external abort. These sit below the real models, so they only
     * catch devices we don't model and the parts of a page a model doesn't cover.
     */
    static const struct { const char *name; hwaddr base; uint64_t size; } unimp_regions[] = {
        { "sha1-page",     SHA1_MEM_BASE,     0x1000 },
        { "usb-otg-page",  USBOTG_MEM_BASE,   0x10000 },
        { "fmss-page",     FMSS_MEM_BASE,     0x1000 },
        { "aes-page",      AES_MEM_BASE,      0x1000 },
        { "mpvd",          MPVD_MEM_BASE,     0x70000 },
        { "prng",          PRNG_MEM_BASE,     0x1000 },
        { "spi0-page",     SPI0_MEM_BASE,     0x1000 },
        { "i2c0-page",     I2C0_MEM_BASE,     0x1000 },
        { "i2c1-page",     I2C1_MEM_BASE,     0x1000 },
        { "uart0-page",    UART0_MEM_BASE,    0x1000 },
        { "spi1-page",     SPI1_MEM_BASE,     0x1000 },
        { "uart1-page",    UART1_MEM_BASE,    0x1000 },
        { "spi4-page",     SPI4_MEM_BASE,     0x1000 },
        /*
         * Not in the device tree: from iPhone OS 2.2 on, iBSS writes bus arbitration settings here (0x40, 0x44)
         * together with the priority registers of the MIU and SYSIC. Without a backing region they abort.
         */
        { "bus-config",    BUSCFG_MEM_BASE,   0x1000 },
    };
    for (int i = 0; i < ARRAY_SIZE(unimp_regions); i++) {
        create_unimplemented_device(unimp_regions[i].name, unimp_regions[i].base, unimp_regions[i].size);
    }

    // the bootrom (vrom), empty without one, so the RAM layout (and with it a saved state) doesn't depend on it
    allocate_ram(sysmem, "vrom", VROM_MEM_BASE, 0x20000);
    if (nms->bootrom_path[0]) {
        g_autofree uint8_t *file_data = NULL;
        gsize fsize;
        if (!g_file_get_contents(nms->bootrom_path, (char **)&file_data, &fsize, NULL)) {
            error_report("unable to read the bootrom at %s", nms->bootrom_path);
            exit(1);
        }
        address_space_rw(nsas, VROM_MEM_BASE, MEMTXATTRS_UNSPECIFIED, file_data, MIN(fsize, 0x20000), 1);
    }
}

static char *ipod_touch_get_bootrom_path(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return g_strdup(nms->bootrom_path);
}

static void ipod_touch_set_bootrom_path(Object *obj, const char *value, Error **errp)
{
    gboolean bootrom_exists = g_file_test(value, G_FILE_TEST_EXISTS);
    if(!bootrom_exists) {
        error_report("bootrom at path \"%s\" must exist", value);
        exit(1);
    }
    
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    g_strlcpy(nms->bootrom_path, value, sizeof(nms->bootrom_path));
}

static char *ipod_touch_get_nor_path(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return g_strdup(nms->nor_path);
}

static void ipod_touch_set_nor_path(Object *obj, const char *value, Error **errp)
{
    gboolean nor_exists = g_file_test(value, G_FILE_TEST_EXISTS);
    if(!nor_exists) {
        error_report("NOR at path \"%s\" must exist", value);
        exit(1);
    }

    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    g_strlcpy(nms->nor_path, value, sizeof(nms->nor_path));
}

static char *ipod_touch_get_nand_path(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return g_strdup(nms->nand_path);
}

static void ipod_touch_set_nand_path(Object *obj, const char *value, Error **errp)
{
    gboolean nand_exists = g_file_test(value, G_FILE_TEST_IS_DIR);
    if(!nand_exists) {
        error_report("NAND at path \"%s\" must be a directory", value);
        exit(1);
    }
    
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    g_strlcpy(nms->nand_path, value, sizeof(nms->nand_path));
}

static char *ipod_touch_get_nand_disk(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return g_strdup(nms->nand_disk);
}

static void ipod_touch_set_nand_disk(Object *obj, const char *value, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    g_free(nms->nand_disk);
    nms->nand_disk = g_strdup(value);
}

static char *ipod_touch_get_nand_overlay(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return g_strdup(nms->nand_overlay);
}

static void ipod_touch_set_nand_overlay(Object *obj, const char *value, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    g_free(nms->nand_overlay);
    nms->nand_overlay = g_strdup(value);
}

static char *ipod_touch_get_usb_chardev(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return g_strdup(nms->usb_chardev);
}

static void ipod_touch_set_usb_chardev(Object *obj, const char *value, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    g_free(nms->usb_chardev);
    nms->usb_chardev = g_strdup(value);
}

static char *ipod_touch_get_netdev(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return g_strdup(nms->netdev);
}

static void ipod_touch_set_netdev(Object *obj, const char *value, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    g_free(nms->netdev);
    nms->netdev = g_strdup(value);
}

static char *ipod_touch_get_boot_args(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return g_strdup(nms->boot_args);
}

static void ipod_touch_set_boot_args(Object *obj, const char *value, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    g_free(nms->boot_args);
    nms->boot_args = g_strdup(value);
}

static bool ipod_touch_get_force_dfu(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return nms->force_dfu;
}

static void ipod_touch_set_force_dfu(Object *obj, bool value, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    nms->force_dfu = value;
}

static char *ipod_touch_get_boot_image(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return g_strdup(nms->boot_image_path);
}

static void ipod_touch_set_boot_image(Object *obj, const char *value, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    g_free(nms->boot_image_path);
    nms->boot_image_path = g_strdup(value);
}

static void ipod_touch_instance_init(Object *obj)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);

    object_property_add_str(obj, "bootrom", ipod_touch_get_bootrom_path, ipod_touch_set_bootrom_path);
    object_property_set_description(obj, "bootrom", "Path to the S5L8720 bootrom binary (optional: without it the "
                                                    "machine starts LLB from the NOR, or boot-image, itself)");

    object_property_add_str(obj, "boot-image", ipod_touch_get_boot_image, ipod_touch_set_boot_image);
    object_property_set_description(obj, "boot-image", "IMG3 image to start instead of LLB from the NOR, e.g. iBSS "
                                                       "for a restore (without a bootrom only)");

	object_property_add_str(obj, "nor", ipod_touch_get_nor_path, ipod_touch_set_nor_path);
    object_property_set_description(obj, "nor", "Path to the S5L8720 NOR image");

    object_property_add_str(obj, "nand", ipod_touch_get_nand_path, ipod_touch_set_nand_path);
    object_property_set_description(obj, "nand", "Path to the NAND files");

    object_property_add_str(obj, "nand-disk", ipod_touch_get_nand_disk, ipod_touch_set_nand_disk);
    object_property_set_description(obj, "nand-disk", "Disk image presented as a freshly formatted NAND "
                                                      "(instead of nand)");

    object_property_add_str(obj, "nand-overlay", ipod_touch_get_nand_overlay, ipod_touch_set_nand_overlay);
    object_property_set_description(obj, "nand-overlay", "Directory for the NAND pages written on top of nand-disk");

    nms->nand_epoch = 1;
    object_property_add_uint32_ptr(obj, "nand-epoch", &nms->nand_epoch, OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "nand-epoch", "Security epoch of the NAND driver the nand-disk NAND is "
                                                       "formatted for: 1 for iPhone OS 2.1.1, 2 for 2.2");

    object_property_add_str(obj, "usb-chardev", ipod_touch_get_usb_chardev, ipod_touch_set_usb_chardev);
    object_property_set_description(obj, "usb-chardev", "ID of the chardev that carries the USB link to the host");

    object_property_add_str(obj, "netdev", ipod_touch_get_netdev, ipod_touch_set_netdev);
    object_property_set_description(obj, "netdev", "ID of the network backend of the Wi-Fi card");

    object_property_add_str(obj, "boot-args", ipod_touch_get_boot_args, ipod_touch_set_boot_args);
    object_property_set_description(obj, "boot-args", "Kernel boot arguments iBoot passes on a normal boot (default: "
                                                      "the debug arguments for iPhone OS 2.1.1, none for later "
                                                      "releases)");

    object_property_add_bool(obj, "dfu", ipod_touch_get_force_dfu, ipod_touch_set_force_dfu);
    object_property_set_description(obj, "dfu", "Hold the force-DFU GPIO so the bootrom enters DFU mode (with bootrom only)");
}

static inline qemu_irq s5l8900_get_irq(IPodTouchMachineState *s, int n)
{
    return s->irq[n / S5L8720_VIC_SIZE][n % S5L8720_VIC_SIZE];
}

static uint32_t s5l8720_usb_hwcfg[] = {
    0,
    0x7a8f60d0,
    0x082000e8,
    0x01f08024
};

static void ipod_touch_key_event(void *opaque, int keycode)
{
    bool do_irq = false;
    int gpio_group = 0, gpio_selector = 0;

    IPodTouchMultitouchState *s = (IPodTouchMultitouchState *)opaque;
    if(keycode == KEY_P_DOWN || keycode == KEY_P_UP) {
        // power button
        gpio_group = GPIO_BUTTON_POWER_IRQ / NUM_GPIO_PINS;
        gpio_selector = GPIO_BUTTON_POWER_IRQ % NUM_GPIO_PINS;

        if(keycode == KEY_P_DOWN && gpio_is_off(s->gpio_state->gpio_state, GPIO_BUTTON_POWER)) {
            gpio_set_on(s->gpio_state->gpio_state, GPIO_BUTTON_POWER);
            
        }
        else if(keycode == KEY_P_UP) {
            gpio_set_off(s->gpio_state->gpio_state, GPIO_BUTTON_POWER);
        }
    }
    else if(keycode == KEY_H_DOWN || keycode == KEY_H_UP) {
        // home button
        gpio_group = GPIO_BUTTON_HOME_IRQ / NUM_GPIO_PINS;
        gpio_selector = GPIO_BUTTON_HOME_IRQ % NUM_GPIO_PINS;

        if(keycode == KEY_H_DOWN && gpio_is_off(s->gpio_state->gpio_state, GPIO_BUTTON_HOME)) {
            gpio_set_on(s->gpio_state->gpio_state, GPIO_BUTTON_HOME);
        }
        else if(keycode == KEY_H_UP) {
            gpio_set_off(s->gpio_state->gpio_state, GPIO_BUTTON_HOME);
        }
    }
    else if(keycode == KEY_MIN_DOWN || keycode == KEY_MIN_UP) {
        // volume down button
        gpio_group = GPIO_BUTTON_VOLDOWN_IRQ / NUM_GPIO_PINS;
        gpio_selector = GPIO_BUTTON_VOLDOWN_IRQ % NUM_GPIO_PINS;

        // the volume buttons are active low
        if(keycode == KEY_MIN_DOWN && gpio_is_on(s->gpio_state->gpio_state, GPIO_BUTTON_VOLDOWN)) {
            gpio_set_off(s->gpio_state->gpio_state, GPIO_BUTTON_VOLDOWN);
        }
        else if(keycode == KEY_MIN_UP) {
            gpio_set_on(s->gpio_state->gpio_state, GPIO_BUTTON_VOLDOWN);
        }
    }
    else if(keycode == KEY_PLUS_DOWN || keycode == KEY_PLUS_UP) {
        // volume up button, active low
        gpio_group = GPIO_BUTTON_VOLUP_IRQ / NUM_GPIO_PINS;
        gpio_selector = GPIO_BUTTON_VOLUP_IRQ % NUM_GPIO_PINS;

        if(keycode == KEY_PLUS_DOWN && gpio_is_on(s->gpio_state->gpio_state, GPIO_BUTTON_VOLUP)) {
            gpio_set_off(s->gpio_state->gpio_state, GPIO_BUTTON_VOLUP);
        }
        else if(keycode == KEY_PLUS_UP) {
            gpio_set_on(s->gpio_state->gpio_state, GPIO_BUTTON_VOLUP);
        }
    }
    else if(keycode == KEY_M_DOWN) {
        // the iPod Touch 2G has no mute switch: mute the host audio output instead
        IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(qdev_get_machine());
        bool muted = ipod_touch_i2s_toggle_host_mute(nms->i2s_state);
        info_report("iPod Touch: host audio %s", muted ? "muted" : "unmuted");
        return;
    }
    else if(keycode == KEY_R_DOWN) {
        // rotate the device by tilting the accelerometer
        IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(qdev_get_machine());
        info_report("iPod Touch: rotated to %s", lis302dl_rotate(nms->accelerometer));
        return;
    }
    else return;
    
    s->sysic->gpio_int_status[gpio_group] |= (1 << gpio_selector);
    qemu_irq_raise(s->sysic->gpio_irqs[gpio_group]);
}

/*
 * The button keys are matched on their Linux key codes, which for these keys
 * equal the PC scancodes the handler was written against.
 */
static void ipod_touch_key_input_event(DeviceState *dev, QemuConsole *src,
                                       QemuInputEvent *evt)
{
    ipod_touch_key_event(dev, evt->key.key | (evt->key.down ? 0 : KEY_UP));
}

static const QemuInputHandler ipod_touch_key_handler = {
    .name  = "iPod Touch Buttons",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = ipod_touch_key_input_event,
};

// The machine's own state: the cp15 register the CPU model lacks
static const VMStateDescription vmstate_ipod_touch_machine = {
    .name = "ipodtouch.machine",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(periph_port_remap, IPodTouchMachineState),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_machine_init(MachineState *machine)
{
	IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(machine);
	MemoryRegion *sysmem;
    AddressSpace *nsas;
    ARMCPU *cpu;

    ipod_touch_cpu_setup(machine, &sysmem, &cpu, &nsas);

    nms->cpu = cpu;
    nms->nsas = nsas;

    // setup VICs
    nms->irq = g_malloc0(sizeof(qemu_irq *) * 2);
    DeviceState *dev = pl192_manual_init("vic0", qdev_get_gpio_in(DEVICE(nms->cpu), ARM_CPU_IRQ), qdev_get_gpio_in(DEVICE(nms->cpu), ARM_CPU_FIQ), NULL);
    PL192State *s = PL192(dev);
    nms->vic0 = s;
    memory_region_add_subregion(sysmem, VIC0_MEM_BASE, &nms->vic0->iomem);
    nms->irq[0] = g_malloc0(sizeof(qemu_irq) * 32);
    for (int i = 0; i < 32; i++) { nms->irq[0][i] = qdev_get_gpio_in(dev, i); }

    dev = pl192_manual_init("vic1", NULL);
    s = PL192(dev);
    nms->vic1 = s;
    memory_region_add_subregion(sysmem, VIC1_MEM_BASE, &nms->vic1->iomem);
    nms->irq[1] = g_malloc0(sizeof(qemu_irq) * 32);
    for (int i = 0; i < 32; i++) { nms->irq[1][i] = qdev_get_gpio_in(dev, i); }

    // chain VICs together
    nms->vic1->daisy = nms->vic0;
    nms->vic0->daisy_callback = nms->vic1;

    // init clock 0
    dev = qdev_new(TYPE_IPOD_TOUCH_CLOCK);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, CLOCK0_MEM_BASE);
    nms->clock0 = IPOD_TOUCH_CLOCK(dev);

    // init clock 1
    dev = qdev_new(TYPE_IPOD_TOUCH_CLOCK);
    qdev_prop_set_bit(dev, "bus-block", true);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, CLOCK1_MEM_BASE);
    nms->clock1 = IPOD_TOUCH_CLOCK(dev);

    // init the timer
    dev = sysbus_create_simple(TYPE_S5L_TIMER, TIMER1_MEM_BASE, s5l8900_get_irq(nms, S5L8720_TIMER1_IRQ));
    nms->timer1 = S5L_TIMER(dev);

    // init the watchdog timer
    sysbus_create_simple(TYPE_IPOD_TOUCH_WDT, WDT_MEM_BASE, s5l8900_get_irq(nms, S5L8720_WDT_IRQ));

    // init sysic
    dev = qdev_new("s5l.sysic");
    S5LSYSICState *sysic_state = S5L_SYSIC(dev);
    nms->sysic = (S5LSYSICState *) g_malloc0(sizeof(struct S5LSYSICState));
    memory_region_add_subregion(sysmem, SYSIC_MEM_BASE, &sysic_state->iomem);
    SysBusDevice *busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize(busdev, &error_fatal);
    for(int grp = 0; grp < ARRAY_SIZE(S5L8900_GPIO_IRQS); grp++) {
        sysbus_connect_irq(busdev, grp, s5l8900_get_irq(nms, S5L8900_GPIO_IRQS[grp]));
    }

    // init GPIO
    dev = qdev_new("s5l.gpio");
    S5LGPIOState *gpio_state = S5L_GPIO(dev);
    nms->gpio_state = gpio_state;
    memory_region_add_subregion(sysmem, GPIO_MEM_BASE, &gpio_state->iomem);
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);

    // init SDIO, with the Wi-Fi card on the netdev= backend or on the first -nic
    dev = qdev_new("s5l.sdio");
    S5LSDIOState *sdio_state = S5L_SDIO(dev);
    nms->sdio_state = sdio_state;
    if (nms->netdev) {
        NetClientState *netdev = qemu_find_netdev(nms->netdev);
        if (!netdev) {
            error_report("netdev '%s' not found", nms->netdev);
            exit(1);
        }
        qdev_prop_set_netdev(dev, "netdev", netdev);
    } else {
        qemu_configure_nic_device(dev, true, "bcm4325");
    }
    memory_region_add_subregion(sysmem, SDIO_MEM_BASE, &sdio_state->iomem);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_SDIO_IRQ));

    // init the UARTs: Samsung UARTs like the Exynos one, but with the S5L8720 interrupt registers
    const hwaddr uart_bases[] = { UART0_MEM_BASE, UART1_MEM_BASE, UART2_MEM_BASE, UART3_MEM_BASE };
    DeviceState *uarts[ARRAY_SIZE(uart_bases)];
    for (int i = 0; i < ARRAY_SIZE(uart_bases); i++) {
        dev = qdev_new("exynos4210.uart");
        qdev_prop_set_chr(dev, "chardev", serial_hd(i));
        qdev_prop_set_uint32(dev, "channel", i);
        qdev_prop_set_uint32(dev, "rx-size", 256);
        qdev_prop_set_uint32(dev, "tx-size", 256);
        qdev_prop_set_bit(dev, "s5l8720-irq", true);
        busdev = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(busdev, &error_fatal);
        sysbus_mmio_map(busdev, 0, uart_bases[i]);
        sysbus_connect_irq(busdev, 0, nms->irq[0][24 + i]);
        uarts[i] = dev;
    }

    // dev = exynos4210_uart_create(UART4_MEM_BASE, 256, 4, serial_hd(4), nms->irq[0][28]);
    // if (!dev) {
    //     printf("Failed to create uart4 device!\n");
    //     abort();
    // }

    // init spis
    set_spi_base(0);
    dev = sysbus_create_simple("s5l.spi", SPI0_MEM_BASE, s5l8900_get_irq(nms, S5L8720_SPI0_IRQ));
    S5LSPIState *spi0_state = S5L_SPI(dev);
    nms->spi0_state = spi0_state;

    set_spi_base(1);
    dev = sysbus_create_simple("s5l.spi", SPI1_MEM_BASE, s5l8900_get_irq(nms, S5L8720_SPI1_IRQ));
    S5LSPIState *spi1_state = S5L_SPI(dev);
    nms->spi1_state = spi1_state;

    // the bootrom and iBoot reach the NOR through SPI0, the kernel through SPI1
    IPodTouchNORSPIState *nor0 = IPOD_TOUCH_NOR_SPI(ssi_create_peripheral(spi0_state->spi, TYPE_IPOD_TOUCH_NOR_SPI));
    IPodTouchNORSPIState *nor1 = IPOD_TOUCH_NOR_SPI(ssi_create_peripheral(spi1_state->spi, TYPE_IPOD_TOUCH_NOR_SPI));

    // both SPI buses are connected to the same NOR, selected through GPIOs
    IPodTouchNORImage *nor_image = ipod_touch_nor_image_load(nms->nor_path);
    nms->nor_image = nor_image;
    nor0->image = nor_image;
    nor1->image = nor_image;
    qdev_connect_gpio_out(DEVICE(gpio_state), GPIO_OUT_INDEX(GPIO_NOR_CS_SPI0), qdev_get_gpio_in_named(DEVICE(nor0), SSI_GPIO_CS, 0));
    qdev_connect_gpio_out(DEVICE(gpio_state), GPIO_OUT_INDEX(GPIO_NOR_CS_SPI1), qdev_get_gpio_in_named(DEVICE(nor1), SSI_GPIO_CS, 0));

    set_spi_base(2);
    sysbus_create_simple("s5l.spi", SPI2_MEM_BASE, s5l8900_get_irq(nms, S5L8720_SPI2_IRQ));

    set_spi_base(3);
    sysbus_create_simple("s5l.spi", SPI3_MEM_BASE, s5l8900_get_irq(nms, S5L8720_SPI3_IRQ));

    set_spi_base(4);
    dev = sysbus_create_simple("s5l.spi", SPI4_MEM_BASE, s5l8900_get_irq(nms, S5L8720_SPI4_IRQ));
    S5LSPIState *spi4_state = S5L_SPI(dev);
    IPodTouchMultitouchState *mt = IPOD_TOUCH_MULTITOUCH(ssi_create_peripheral(spi4_state->spi, TYPE_IPOD_TOUCH_MULTITOUCH));
    mt->sysic = sysic_state;
    mt->gpio_state = gpio_state;
    qdev_connect_gpio_out_named(DEVICE(mt), "irq", 0, qdev_get_gpio_in(DEVICE(sysic_state), GPIO_MULTITOUCH_IRQ));
    qdev_connect_gpio_out(DEVICE(gpio_state), GPIO_OUT_INDEX(GPIO_MULTITOUCH_CS), qdev_get_gpio_in_named(DEVICE(mt), SSI_GPIO_CS, 0));
    qdev_connect_gpio_out(DEVICE(gpio_state), GPIO_OUT_INDEX(GPIO_MULTITOUCH_RESET), qdev_get_gpio_in_named(DEVICE(mt), "reset", 0));
    nms->spi4_state = spi4_state;

    // init the chip ID module
    dev = qdev_new("s5l.chipid");
    S5LChipIDState *chipid_state = S5L_CHIPID(dev);
    nms->chipid_state = chipid_state;
    memory_region_add_subregion(sysmem, CHIPID_MEM_BASE, &chipid_state->iomem);
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);

    // init the TVOut instance
    dev = qdev_new("s5l.tvout");
    S5LTVOutState *tvout_state = S5L_TVOUT(dev);
    nms->tvout_state = tvout_state;
    memory_region_add_subregion(sysmem, TVOUT_MIXER1_MEM_BASE, &tvout_state->mixer1_iomem);
    memory_region_add_subregion(sysmem, TVOUT_MIXER2_MEM_BASE, &tvout_state->mixer2_iomem);
    memory_region_add_subregion(sysmem, TVOUT_SDO_MEM_BASE, &tvout_state->sdo_iomem);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_TVOUT_SDO_IRQ));

    // init the MIU (SDRAM controller)
    dev = qdev_new(TYPE_IPOD_TOUCH_MIU);
    IPodTouchMIUState *miu_state = IPOD_TOUCH_MIU(dev);
    memory_region_add_subregion(sysmem, MIU_MEM_BASE, &miu_state->iomem);
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);

    // init USB OTG
    dev = ipod_touch_init_usb_otg(s5l8900_get_irq(nms, S5L8720_USB_OTG_IRQ), s5l8720_usb_hwcfg);
    synopsys_usb_state *usb_otg = S5L_USB_OTG(dev);
    nms->usb_otg = usb_otg;
    if (nms->usb_chardev) {
        Chardev *usb_chr = qemu_chr_find(nms->usb_chardev);
        if (!usb_chr) {
            error_report("USB chardev '%s' not found", nms->usb_chardev);
            exit(1);
        }
        qdev_prop_set_chr(dev, "chardev", usb_chr);
    }
    memory_region_add_subregion(sysmem, USBOTG_MEM_BASE, &nms->usb_otg->iomem);
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);

    // init two pl080 DMAC0 devices
    dev = qdev_new("pl080");
    PL080State *pl080_1 = PL080(dev);
    object_property_set_link(OBJECT(dev), "downstream", OBJECT(sysmem), &error_fatal);
    qdev_prop_set_uint32(dev, "dreq-mask", (1 << DMAC0_I2S0_TX_DREQ) | (1 << DMAC0_I2S0_RX_DREQ) |
                         (1 << DMAC0_UART0_RX_DREQ) | (1 << DMAC0_UART1_RX_DREQ));
    memory_region_add_subregion(sysmem, DMAC0_MEM_BASE, &pl080_1->iomem1);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_DMAC0_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(uarts[0]), 1, qdev_get_gpio_in_named(DEVICE(pl080_1), "dreq", DMAC0_UART0_RX_DREQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(uarts[1]), 1, qdev_get_gpio_in_named(DEVICE(pl080_1), "dreq", DMAC0_UART1_RX_DREQ));

    dev = qdev_new("pl080");
    PL080State *pl080_2 = PL080(dev);
    object_property_set_link(OBJECT(dev), "downstream", OBJECT(sysmem), &error_fatal);
    memory_region_add_subregion(sysmem, DMAC1_0_MEM_BASE, &pl080_2->iomem1);
    memory_region_add_subregion(sysmem, DMAC1_1_MEM_BASE, &pl080_2->iomem2);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_DMAC1_IRQ));

    // init the I2S controller, which feeds the audio codec
    dev = qdev_new(TYPE_IPOD_TOUCH_I2S);
    nms->i2s_state = IPOD_TOUCH_I2S(dev);
    if (machine->audiodev) {
        qdev_prop_set_string(dev, "audiodev", machine->audiodev);
    }
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(busdev, &error_fatal);
    sysbus_mmio_map(busdev, 0, I2S0_MEM_BASE);
    qdev_connect_gpio_out_named(dev, "sync", 0, qdev_get_gpio_in(DEVICE(sysic_state), GPIO_I2S0_IRQ));
    qdev_connect_gpio_out_named(dev, "dma-tx", 0, qdev_get_gpio_in_named(DEVICE(pl080_1), "dreq", DMAC0_I2S0_TX_DREQ));
    qdev_connect_gpio_out_named(dev, "dma-rx", 0, qdev_get_gpio_in_named(DEVICE(pl080_1), "dreq", DMAC0_I2S0_RX_DREQ));

    // init the AMC audio decoder, whose SRAM is the start of the "llb" RAM
    dev = qdev_new(TYPE_IPOD_TOUCH_AMC);
    nms->amc_state = IPOD_TOUCH_AMC(dev);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(busdev, &error_fatal);
    sysbus_mmio_map(busdev, 0, AMC_MEM_BASE);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_AMC_IRQ));

    // Init I2C0
    dev = qdev_new("ipodtouch.i2c");
    IPodTouchI2CState *i2c_state = IPOD_TOUCH_I2C(dev);
    i2c_state->base = 0;
    nms->i2c0_state = i2c_state;
    busdev = SYS_BUS_DEVICE(dev);
    memory_region_add_subregion(sysmem, I2C0_MEM_BASE, &i2c_state->iomem);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_I2C0_IRQ));

    // init the PMU
    I2CSlave * pmu = i2c_slave_create_simple(i2c_state->bus, "pcf50633", 0x73);
    PCF50633(pmu)->usb_present = nms->usb_chardev != NULL;

    // init the accelerometer
    I2CSlave *accelerometer = i2c_slave_new("lis302dl", 0x1D);
    object_property_add_child(OBJECT(machine), "accelerometer", OBJECT(accelerometer));
    nms->accelerometer = LIS302DL(accelerometer);
    i2c_slave_realize_and_unref(accelerometer, i2c_state->bus, &error_fatal);

    // init the audio codec, which plays what the I2S controller sends
    I2CSlave *codec = i2c_slave_new(TYPE_CS42L58, 0x4A);
    object_property_set_link(OBJECT(codec), "i2s", OBJECT(nms->i2s_state), &error_fatal);
    i2c_slave_realize_and_unref(codec, i2c_state->bus, &error_fatal);

    // Init I2C1
    dev = qdev_new("ipodtouch.i2c");
    i2c_state = IPOD_TOUCH_I2C(dev);
    nms->i2c1_state = i2c_state;
    i2c_state->base = 1;
    busdev = SYS_BUS_DEVICE(dev);
    memory_region_add_subregion(sysmem, I2C1_MEM_BASE, &i2c_state->iomem);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_I2C1_IRQ));
    
    // Init the light sensor
    I2CSlave *isl29003dl = i2c_slave_new("isl29003dl", 0x44);
    object_property_add_child(OBJECT(machine), "lightsensor", OBJECT(isl29003dl));
    i2c_slave_realize_and_unref(isl29003dl, i2c_state->bus, &error_fatal);
    qdev_connect_gpio_out(DEVICE(isl29003dl), 0, qdev_get_gpio_in(DEVICE(sysic_state), GPIO_LIGHTSENSOR_IRQ));

    // init the Mikey
    I2CSlave *cd327mikey = i2c_slave_create_simple(i2c_state->bus, "cd3272mikey", 0x39);

    // init the FMSS flash controller
    dev = qdev_new("ipodtouch.fmss");
    IPodTouchFMSSState *fmss_state = IPOD_TOUCH_FMSS(dev);
    if (nms->nand_disk) {
        if (!nms->nand_overlay || nms->nand_path[0]) {
            error_report("nand-disk needs nand-overlay, and can't be combined with nand");
            exit(1);
        }
        ipod_touch_nand_open_disk(&fmss_state->nand, nms->nand_disk, nms->nand_overlay, nms->nand_epoch,
                                  &error_fatal);
    } else {
        ipod_touch_nand_open_pages(&fmss_state->nand, nms->nand_path[0] ? nms->nand_path : NULL);
    }
    fmss_state->boot_args = nms->boot_args;
    nms->fmss_state = fmss_state;
    busdev = SYS_BUS_DEVICE(dev);
    memory_region_add_subregion(sysmem, FMSS_MEM_BASE, &fmss_state->iomem);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_FMSS_IRQ));

    // init the USB module
    dev = qdev_new("s5l.usbphys");
    S5LUSBPhysState *usb_phys_state = S5L_USB_PHYS(dev);
    nms->usb_phys_state = usb_phys_state;
    usb_phys_state->cable_connected = nms->usb_chardev != NULL;
    memory_region_add_subregion(sysmem, USBPHYS_MEM_BASE, &usb_phys_state->iomem);
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);

    ipod_touch_memory_setup(machine, sysmem, nsas);

    // init the MIPI DSI controller
    dev = qdev_new("ipodtouch.mipidsi");
    nms->mipi_dsi_state = IPOD_TOUCH_MIPI_DSI(dev);
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, MIPI_DSI_MEM_BASE);

    // init LCD
    dev = qdev_new("ipodtouch.lcd");
    IPodTouchLCDState *lcd_state = IPOD_TOUCH_LCD(dev);
    lcd_state->sysmem = sysmem;
    lcd_state->mt = mt;
    nms->lcd_state = lcd_state;
    busdev = SYS_BUS_DEVICE(dev);
    memory_region_add_subregion(sysmem, DISPLAY_MEM_BASE, &lcd_state->iomem);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_LCD_IRQ));

    // init scaler / CSC
    dev = qdev_new("ipodtouch.scalercsc");
    IPodTouchScalerCSCState *scaler_csc_state = IPOD_TOUCH_SCALER_CSC(dev);
    nms->scaler_csc_state = scaler_csc_state;
    busdev = SYS_BUS_DEVICE(dev);
    memory_region_add_subregion(sysmem, SCALER_CSC_MEM_BASE, &scaler_csc_state->iomem);
    sysbus_realize(busdev, &error_fatal);

    // init SHA1 engine
    dev = qdev_new("s5l.sha1");
    S5LSHA1State *sha1_state = S5L_SHA1(dev);
    nms->sha1_state = sha1_state;
    busdev = SYS_BUS_DEVICE(dev);
    memory_region_add_subregion(sysmem, SHA1_MEM_BASE, &sha1_state->iomem);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_SHA1_IRQ));
    sysbus_realize(busdev, &error_fatal);

    // init AES engine
    dev = qdev_new("ipodtouch.aes");
    IPodTouchAESState *aes_state = IPOD_TOUCH_AES(dev);
    nms->aes_state = aes_state;
    memory_region_add_subregion(sysmem, AES_MEM_BASE, &aes_state->iomem);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, s5l8900_get_irq(nms, S5L8720_AES_IRQ));
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);

    // init PKE engine
    dev = qdev_new("ipodtouch.pke");
    IPodTouchPKEState *pke_state = IPOD_TOUCH_PKE(dev);
    nms->pke_state = pke_state;
    memory_region_add_subregion(sysmem, PKE_MEM_BASE, &pke_state->iomem);
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);

    // init the MBX
    dev = qdev_new("s5l.mbx");
    S5LMBXState *mbx_state = S5L_MBX(dev);
    nms->mbx_state = mbx_state;
    busdev = SYS_BUS_DEVICE(dev);
    memory_region_add_subregion(sysmem, MBX1_MEM_BASE, &mbx_state->iomem1);
    memory_region_add_subregion(sysmem, MBX2_MEM_BASE, &mbx_state->iomem2);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_MBX_IRQ));

    if (nms->bootrom_path[0]) {
        if (nms->boot_image_path) {
            error_report("boot-image starts a boot stage without the bootrom; it can't be combined with bootrom");
            exit(1);
        }
    } else {
        if (nms->force_dfu) {
            error_report("dfu needs the bootrom (to start iBSS without one, pass it as boot-image)");
            exit(1);
        }
        if (nms->boot_image_path &&
            !g_file_get_contents(nms->boot_image_path, (char **)&nms->boot_image, &nms->boot_image_size, NULL)) {
            error_report("unable to read the boot image at %s", nms->boot_image_path);
            exit(1);
        }
        // check the image now rather than at reset
        Error *err = NULL;
        if (!ipod_touch_load_first_stage(nms, &err)) {
            error_report_err(err);
            exit(1);
        }
    }

    qemu_register_reset(ipod_touch_cpu_reset, nms);
    vmstate_register(NULL, 0, &vmstate_ipod_touch_machine, nms);

    QemuInputHandlerState *key_handler =
        qemu_input_handler_register(DEVICE(mt), &ipod_touch_key_handler);
    qemu_input_handler_activate(key_handler);

    if (nms->force_dfu) {
        gpio_set_on(nms->gpio_state->gpio_state, GPIO_FORCE_DFU);
    }

    // The volume buttons are active low (their device tree GPIO functions lack the 0x100 flag of the home and power
    // buttons): released, they read high.
    gpio_set_on(nms->gpio_state->gpio_state, GPIO_BUTTON_VOLUP);
    gpio_set_on(nms->gpio_state->gpio_state, GPIO_BUTTON_VOLDOWN);
}

static void ipod_touch_machine_class_init(ObjectClass *klass, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(klass);
    mc->desc = "iPod Touch";
    mc->init = ipod_touch_machine_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm1176");
    machine_add_audiodev_property(mc);
}

static const TypeInfo ipod_touch_machine_info = {
    .name          = TYPE_IPOD_TOUCH_MACHINE,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(IPodTouchMachineState),
    .class_size    = sizeof(IPodTouchMachineClass),
    .class_init    = ipod_touch_machine_class_init,
    .instance_init = ipod_touch_instance_init,
    .interfaces    = arm_machine_interfaces,
};

static void ipod_touch_machine_types(void)
{
    type_register_static(&ipod_touch_machine_info);
}

type_init(ipod_touch_machine_types)
