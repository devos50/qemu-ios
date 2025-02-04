#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/boot.h"
#include "exec/address-spaces.h"
#include "hw/misc/unimp.h"
#include "hw/irq.h"
#include "sysemu/sysemu.h"
#include "sysemu/reset.h"
#include "hw/platform-bus.h"
#include "hw/block/flash.h"
#include "hw/qdev-clock.h"
#include "hw/arm/exynos4210.h"
#include "hw/arm/ipod_touch_2g.h"
#include "target/arm/cpregs.h"
#include "qemu/error-report.h"

#define VMSTATE_IT2G_CPREG(name) \
        VMSTATE_UINT64(IT2G_CPREG_VAR_NAME(name), IPodTouchMachineState)

#define IT2G_CPREG_DEF(p_name, p_op0, p_op1, p_crn, p_crm, p_op2, p_access, p_reset) \
    {                                                                              \
        .cp = 15,                                              \
        .name = #p_name, .opc0 = p_op0, .crn = p_crn, .crm = p_crm,                \
        .opc1 = p_op1, .opc2 = p_op2, .access = p_access, .resetvalue = p_reset,   \
        .state = ARM_CP_STATE_AA32, .type = ARM_CP_OVERRIDE,                       \
        .fieldoffset = offsetof(IPodTouchMachineState, IT2G_CPREG_VAR_NAME(p_name))           \
                       - offsetof(ARMCPU, env)                                     \
    }

static void allocate_ram(MemoryRegion *top, const char *name, uint32_t addr, uint32_t size)
{
    MemoryRegion *sec = g_new(MemoryRegion, 1);
    memory_region_init_ram(sec, NULL, name, size, &error_fatal);
    memory_region_add_subregion(top, addr, sec);
}

static const ARMCPRegInfo it2g_cp_reginfo_tcg[] = {
    IT2G_CPREG_DEF(REG0, 0, 0, 7, 6, 0, PL1_RW, 0),
    IT2G_CPREG_DEF(REG1, 0, 0, 15, 2, 4, PL1_RW, 0),
    IT2G_CPREG_DEF(REG1, 0, 0, 7, 14, 0, PL1_RW, 0),
    IT2G_CPREG_DEF(REG1, 0, 0, 7, 10, 0, PL1_RW, 0),
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

static void ipod_touch_cpu_reset(void *opaque)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE((MachineState *)opaque);
    ARMCPU *cpu = nms->cpu;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);

    //env->regs[0] = nms->kbootargs_pa;
    //cpu_set_pc(CPU(cpu), 0xc00607ec);
    cpu_set_pc(CPU(cpu), VROM_MEM_BASE);
    //env->regs[0] = 0x9000000;
    //cpu_set_pc(CPU(cpu), LLB_BASE + 0x100);
    //cpu_set_pc(CPU(cpu), VROM_MEM_BASE);
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

    // load the bootrom (vrom)
    uint8_t *file_data = NULL;
    gsize fsize;
    if (g_file_get_contents(nms->bootrom_path, (char **)&file_data, &fsize, NULL)) {
        allocate_ram(sysmem, "vrom", 0x0, 0x20000);
        address_space_rw(nsas, VROM_MEM_BASE, MEMTXATTRS_UNSPECIFIED, (uint8_t *)file_data, fsize, 1);
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

static void ipod_touch_instance_init(Object *obj)
{
    object_property_add_str(obj, "bootrom", ipod_touch_get_bootrom_path, ipod_touch_set_bootrom_path);
    object_property_set_description(obj, "bootrom", "Path to the S5L8720 bootrom binary");

	object_property_add_str(obj, "nor", ipod_touch_get_nor_path, ipod_touch_set_nor_path);
    object_property_set_description(obj, "nor", "Path to the S5L8720 NOR image");

    object_property_add_str(obj, "nand", ipod_touch_get_nand_path, ipod_touch_set_nand_path);
    object_property_set_description(obj, "nand", "Path to the NAND files");
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

        if(keycode == KEY_MIN_DOWN && gpio_is_off(s->gpio_state->gpio_state, GPIO_BUTTON_VOLDOWN)) {
            gpio_set_on(s->gpio_state->gpio_state, GPIO_BUTTON_VOLDOWN);
        }
        else if(keycode == KEY_MIN_UP) {
            gpio_set_off(s->gpio_state->gpio_state, GPIO_BUTTON_VOLDOWN);
        }
    }
    else if(keycode == KEY_PLUS_DOWN || keycode == KEY_PLUS_UP) {
        // volume down button
        gpio_group = GPIO_BUTTON_VOLUP_IRQ / NUM_GPIO_PINS;
        gpio_selector = GPIO_BUTTON_VOLUP_IRQ % NUM_GPIO_PINS;

        if(keycode == KEY_PLUS_DOWN && gpio_is_off(s->gpio_state->gpio_state, GPIO_BUTTON_VOLUP)) {
            gpio_set_on(s->gpio_state->gpio_state, GPIO_BUTTON_VOLUP);
        }
        else if(keycode == KEY_PLUS_UP) {
            gpio_set_off(s->gpio_state->gpio_state, GPIO_BUTTON_VOLUP);
        }
    }
    else return;
    
    s->sysic->gpio_int_status[gpio_group] |= (1 << gpio_selector);
    qemu_irq_raise(s->sysic->gpio_irqs[gpio_group]);
}

static void ipod_touch_machine_init(MachineState *machine)
{
	IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(machine);
	MemoryRegion *sysmem;
    AddressSpace *nsas;
    ARMCPU *cpu;

    ipod_touch_cpu_setup(machine, &sysmem, &cpu, &nsas);

    // setup clock
    nms->sysclk = clock_new(OBJECT(machine), "SYSCLK");
    clock_set_hz(nms->sysclk, 12000000ULL);

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

    // // chain VICs together
    nms->vic1->daisy = nms->vic0;

    // init clock 0
    dev = qdev_new("ipodtouch.clock");
    IPodTouchClockState *clock0_state = IPOD_TOUCH_CLOCK(dev);
    nms->clock0 = clock0_state;
    memory_region_add_subregion(sysmem, CLOCK0_MEM_BASE, &clock0_state->iomem);

    // init clock 1
    dev = qdev_new("ipodtouch.clock");
    IPodTouchClockState *clock1_state = IPOD_TOUCH_CLOCK(dev);
    nms->clock1 = clock1_state;
    memory_region_add_subregion(sysmem, CLOCK1_MEM_BASE, &clock1_state->iomem);

    // init the timer
    dev = qdev_new("ipodtouch.timer");
    IPodTouchTimerState *timer_state = IPOD_TOUCH_TIMER(dev);
    nms->timer1 = timer_state;
    memory_region_add_subregion(sysmem, TIMER1_MEM_BASE, &timer_state->iomem);
    SysBusDevice *busdev = SYS_BUS_DEVICE(dev);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_TIMER1_IRQ));
    //sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_TIMER1_IRQ - 1));
    timer_state->sysclk = nms->sysclk;

    // init sysic
    dev = qdev_new("ipodtouch.sysic");
    IPodTouchSYSICState *sysic_state = IPOD_TOUCH_SYSIC(dev);
    nms->sysic = (IPodTouchSYSICState *) g_malloc0(sizeof(struct IPodTouchSYSICState));
    memory_region_add_subregion(sysmem, SYSIC_MEM_BASE, &sysic_state->iomem);
    busdev = SYS_BUS_DEVICE(dev);
    for(int grp = 0; grp < GPIO_NUMINTGROUPS; grp++) {
        sysbus_connect_irq(busdev, grp, s5l8900_get_irq(nms, S5L8900_GPIO_IRQS[grp]));
    }

    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8900_GPIO_G0_IRQ));
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8900_GPIO_G1_IRQ));
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8900_GPIO_G2_IRQ));
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8900_GPIO_G3_IRQ));
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8900_GPIO_G4_IRQ));

    // init GPIO
    dev = qdev_new("ipodtouch.gpio");
    IPodTouchGPIOState *gpio_state = IPOD_TOUCH_GPIO(dev);
    nms->gpio_state = gpio_state;
    memory_region_add_subregion(sysmem, GPIO_MEM_BASE, &gpio_state->iomem);

    // init SDIO
    dev = qdev_new("ipodtouch.sdio");
    IPodTouchSDIOState *sdio_state = IPOD_TOUCH_SDIO(dev);
    nms->sdio_state = sdio_state;
    memory_region_add_subregion(sysmem, SDIO_MEM_BASE, &sdio_state->iomem);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_SDIO_IRQ));

    dev = exynos4210_uart_create(UART0_MEM_BASE, 256, 0, serial_hd(0), nms->irq[0][24]);
    if (!dev) {
        hw_error("Failed to create UART0 device!");
    }

    dev = exynos4210_uart_create(UART1_MEM_BASE, 256, 1, serial_hd(1), nms->irq[0][25]);
    if (!dev) {
        hw_error("Failed to create UART0 device!");
    }

    dev = exynos4210_uart_create(UART2_MEM_BASE, 256, 2, serial_hd(2), nms->irq[0][26]);
    if (!dev) {
        hw_error("Failed to create UART0 device!");
    }

    dev = exynos4210_uart_create(UART3_MEM_BASE, 256, 3, serial_hd(3), nms->irq[0][27]);
    if (!dev) {
        hw_error("Failed to create UART0 device!");
    }

    // dev = exynos4210_uart_create(UART4_MEM_BASE, 256, 4, serial_hd(4), nms->irq[0][28]);
    // if (!dev) {
    //     printf("Failed to create uart4 device!\n");
    //     abort();
    // }

    // init spis
    set_spi_base(0);
    dev = sysbus_create_simple("ipodtouch.spi", SPI0_MEM_BASE, s5l8900_get_irq(nms, S5L8720_SPI0_IRQ));
    IPodTouchSPIState *spi0_state = IPOD_TOUCH_SPI(dev);
    spi0_state->nor->nor_path = nms->nor_path;
    nms->spi0_state = spi0_state;

    set_spi_base(1);
    dev = sysbus_create_simple("ipodtouch.spi", SPI1_MEM_BASE, s5l8900_get_irq(nms, S5L8720_SPI1_IRQ));
    IPodTouchSPIState *spi1_state = IPOD_TOUCH_SPI(dev);
    nms->spi1_state = spi1_state;

    set_spi_base(2);
    sysbus_create_simple("ipodtouch.spi", SPI2_MEM_BASE, s5l8900_get_irq(nms, S5L8720_SPI2_IRQ));

    set_spi_base(3);
    sysbus_create_simple("ipodtouch.spi", SPI3_MEM_BASE, s5l8900_get_irq(nms, S5L8720_SPI3_IRQ));

    set_spi_base(4);
    dev = sysbus_create_simple("ipodtouch.spi", SPI4_MEM_BASE, s5l8900_get_irq(nms, S5L8720_SPI4_IRQ));
    IPodTouchSPIState *spi4_state = IPOD_TOUCH_SPI(dev);
    spi4_state->mt->sysic = sysic_state;
    spi4_state->mt->gpio_state = gpio_state;
    nms->spi4_state = spi4_state;

    // init the chip ID module
    dev = qdev_new("ipodtouch.chipid");
    IPodTouchChipIDState *chipid_state = IPOD_TOUCH_CHIPID(dev);
    nms->chipid_state = chipid_state;
    memory_region_add_subregion(sysmem, CHIPID_MEM_BASE, &chipid_state->iomem);

    // init the TVOut instance
    dev = qdev_new("ipodtouch.tvout");
    IPodTouchTVOutState *tvout_state = IPOD_TOUCH_TVOUT(dev);
    nms->tvout_state = tvout_state;
    memory_region_add_subregion(sysmem, TVOUT_MIXER1_MEM_BASE, &tvout_state->mixer1_iomem);
    memory_region_add_subregion(sysmem, TVOUT_MIXER2_MEM_BASE, &tvout_state->mixer2_iomem);
    memory_region_add_subregion(sysmem, TVOUT_SDO_MEM_BASE, &tvout_state->sdo_iomem);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_TVOUT_SDO_IRQ));

    // init the unknown1 module
    dev = qdev_new("ipodtouch.unknown1");
    IPodTouchUnknown1State *unknown1_state = IPOD_TOUCH_UNKNOWN1(dev);
    memory_region_add_subregion(sysmem, UNKNOWN1_MEM_BASE, &unknown1_state->iomem);

    // init USB OTG
    dev = ipod_touch_init_usb_otg(s5l8900_get_irq(nms, S5L8720_USB_OTG_IRQ), s5l8720_usb_hwcfg);
    synopsys_usb_state *usb_otg = S5L8900USBOTG(dev);
    nms->usb_otg = usb_otg;
    busdev = SYS_BUS_DEVICE(dev);
    memory_region_add_subregion(sysmem, USBOTG_MEM_BASE, &nms->usb_otg->iomem);
    sysbus_realize(busdev, &error_fatal);

    // init two pl080 DMAC0 devices
    dev = qdev_new("pl080");
    PL080State *pl080_1 = PL080(dev);
    object_property_set_link(OBJECT(dev), "downstream", OBJECT(sysmem), &error_fatal);
    memory_region_add_subregion(sysmem, DMAC0_MEM_BASE, &pl080_1->iomem1);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_DMAC0_IRQ));

    dev = qdev_new("pl080");
    PL080State *pl080_2 = PL080(dev);
    object_property_set_link(OBJECT(dev), "downstream", OBJECT(sysmem), &error_fatal);
    memory_region_add_subregion(sysmem, DMAC1_0_MEM_BASE, &pl080_2->iomem1);
    memory_region_add_subregion(sysmem, DMAC1_1_MEM_BASE, &pl080_2->iomem2);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_DMAC1_IRQ));

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

    // init the accelerometer
    I2CSlave *accelerometer = i2c_slave_create_simple(i2c_state->bus, "lis302dl", 0x1D);

    // init the audio codec (disabled because unused)
    // I2CSlave *audio_codec = i2c_slave_create_simple(i2c_state->bus, "cs42l58", 0x4A);

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
    I2CSlave *isl29003dl = i2c_slave_create_simple(i2c_state->bus, "isl29003dl", 0x44);

    // init the Mikey
    I2CSlave *cd327mikey = i2c_slave_create_simple(i2c_state->bus, "cd3272mikey", 0x39);

    // init the FMSS flash controller
    dev = qdev_new("ipodtouch.fmss");
    IPodTouchFMSSState *fmss_state = IPOD_TOUCH_FMSS(dev);
    fmss_state->nand_path = nms->nand_path;
    nms->fmss_state = fmss_state;
    busdev = SYS_BUS_DEVICE(dev);
    memory_region_add_subregion(sysmem, FMSS_MEM_BASE, &fmss_state->iomem);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_FMSS_IRQ));

    // init the USB module
    dev = qdev_new("ipodtouch.usbphys");
    IPodTouchUSBPhysState *usb_phys_state = IPOD_TOUCH_USB_PHYS(dev);
    nms->usb_phys_state = usb_phys_state;
    memory_region_add_subregion(sysmem, USBPHYS_MEM_BASE, &usb_phys_state->iomem);

    ipod_touch_memory_setup(machine, sysmem, nsas);

    // init the MIPI SDI controller
    dev = qdev_new("ipodtouch.mipidsi");
    IPodTouchMIPIDSIState *mipi_dsi_state = IPOD_TOUCH_MIPI_DSI(dev);
    nms->mipi_dsi_state = mipi_dsi_state;
    memory_region_add_subregion(sysmem, MIPI_DSI_MEM_BASE, &mipi_dsi_state->iomem);

    // init LCD
    dev = qdev_new("ipodtouch.lcd");
    IPodTouchLCDState *lcd_state = IPOD_TOUCH_LCD(dev);
    lcd_state->sysmem = sysmem;
    lcd_state->mt = spi4_state->mt;
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
    dev = qdev_new("ipodtouch.sha1");
    IPodTouchSHA1State *sha1_state = IPOD_TOUCH_SHA1(dev);
    nms->sha1_state = sha1_state;
    memory_region_add_subregion(sysmem, SHA1_MEM_BASE, &sha1_state->iomem);

    // init AES engine
    dev = qdev_new("ipodtouch.aes");
    IPodTouchAESState *aes_state = IPOD_TOUCH_AES(dev);
    nms->aes_state = aes_state;
    memory_region_add_subregion(sysmem, AES_MEM_BASE, &aes_state->iomem);

    // init PKE engine
    dev = qdev_new("ipodtouch.pke");
    IPodTouchPKEState *pke_state = IPOD_TOUCH_PKE(dev);
    nms->pke_state = pke_state;
    memory_region_add_subregion(sysmem, PKE_MEM_BASE, &pke_state->iomem);

    // init the MBX
    dev = qdev_new("ipodtouch.mbx");
    IPodTouchMBXState *mbx_state = IPOD_TOUCH_MBX(dev);
    nms->mbx_state = mbx_state;
    memory_region_add_subregion(sysmem, MBX1_MEM_BASE, &mbx_state->iomem1);
    memory_region_add_subregion(sysmem, MBX2_MEM_BASE, &mbx_state->iomem2);

    qemu_register_reset(ipod_touch_cpu_reset, nms);

    qemu_add_kbd_event_handler(ipod_touch_key_event, spi4_state->mt);

    // toggle DFU
    gpio_set_on(nms->gpio_state->gpio_state, GPIO_FORCE_DFU);
    nms->usb_phys_state->is_cable_connected = true;
}

static void ipod_touch_machine_class_init(ObjectClass *klass, void *data)
{
    MachineClass *mc = MACHINE_CLASS(klass);
    mc->desc = "iPod Touch";
    mc->init = ipod_touch_machine_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm1176");
}

static const TypeInfo ipod_touch_machine_info = {
    .name          = TYPE_IPOD_TOUCH_MACHINE,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(IPodTouchMachineState),
    .class_size    = sizeof(IPodTouchMachineClass),
    .class_init    = ipod_touch_machine_class_init,
    .instance_init = ipod_touch_instance_init,
};

static void ipod_touch_machine_types(void)
{
    type_register_static(&ipod_touch_machine_info);
}

type_init(ipod_touch_machine_types)
