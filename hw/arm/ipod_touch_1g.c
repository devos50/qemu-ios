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
#include "hw/arm/ipod_touch_1g.h"
#include "hw/arm/s5l8900_clock.h"
#include "hw/arm/s5l_chipid.h"
#include "hw/arm/s5l_sha1.h"
#include "hw/arm/ipod_touch_aes.h"
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

#define S5L8900_8900_MAGIC      0x30303938  // '8900'
#define S5L8900_8900_HEADER     0x800
#define S5L8900_8900_ENCRYPTED  3

/*
 * Key 0x837: what the S5L8900's AES engine derives from its GID key, and what 8900 containers are encrypted with
 * (AES-128-CBC, zero IV). It is published, e.g. on The Apple Wiki.
 */
static const uint8_t key_0x837[16] = {
    0x18, 0x84, 0x58, 0xa6, 0xd1, 0x50, 0x34, 0xdf, 0xe3, 0x86, 0xf2, 0x3b, 0x61, 0xd4, 0x37, 0x74,
};

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

/* AES-128-CBC over the whole blocks of buf, in place; a partial last block is left as it is, as the engine does. */
static void it1g_aes_cbc(const uint8_t *key, const uint8_t *iv_in, uint8_t *buf, size_t len, bool encrypt)
{
    uint8_t iv[AES_BLOCK_SIZE], block[AES_BLOCK_SIZE];
    AES_KEY aes;

    memcpy(iv, iv_in, sizeof(iv));
    if (encrypt) {
        AES_set_encrypt_key(key, 128, &aes);
    } else {
        AES_set_decrypt_key(key, 128, &aes);
    }
    for (size_t off = 0; off + AES_BLOCK_SIZE <= len; off += AES_BLOCK_SIZE) {
        if (encrypt) {
            for (int i = 0; i < AES_BLOCK_SIZE; i++) {
                block[i] = buf[off + i] ^ iv[i];
            }
            AES_encrypt(block, buf + off, &aes);
            memcpy(iv, buf + off, AES_BLOCK_SIZE);
        } else {
            memcpy(block, buf + off, AES_BLOCK_SIZE);
            AES_decrypt(block, buf + off, &aes);
            for (int i = 0; i < AES_BLOCK_SIZE; i++) {
                buf[off + i] ^= iv[i];
            }
            memcpy(iv, block, AES_BLOCK_SIZE);
        }
    }
}

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
        it1g_aes_cbc(key_0x837, zero_iv, out, len, false);
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
        it1g_aes_cbc(key_uid, img2_key_iv, key, sizeof(key), true);
        it1g_aes_cbc(key, zero_iv, data, len, false);
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

static void it1g_memory_setup(IPodTouch1GMachineState *nms, MemoryRegion *sysmem)
{
    allocate_ram(sysmem, "ram", IT1G_RAM_MEM_BASE, IT1G_RAM_SIZE);
    // whether this is memory of its own or another window on the DRAM isn't known; its own is the safe guess
    allocate_ram(sysmem, "iboot-ram", IT1G_IBOOT_RAM_BASE, IT1G_IBOOT_RAM_SIZE);
    allocate_ram(sysmem, "vrom", IT1G_VROM_MEM_BASE, 0x10000);
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
        { "dmac0",      IT1G_DMAC0_MEM_BASE,    0x1000 },
        { "usb-otg",    IT1G_USBOTG_MEM_BASE,   0x10000 },
        { "adm",        IT1G_ADM_MEM_BASE,      0x1000 },
        { "display",    IT1G_DISPLAY_MEM_BASE,  0x1000 },
        { "nand",       IT1G_NAND_MEM_BASE,     0x1000 },
        { "sdio",       IT1G_SDIO_MEM_BASE,     0x1000 },
        { "edgeic",     IT1G_EDGEIC_MEM_BASE,   0x1000 },
        { "nand-ecc",   IT1G_NAND_ECC_MEM_BASE, 0x1000 },
        { "tvout1",     IT1G_TVOUT1_MEM_BASE,   0x1000 },
        { "tvout2",     IT1G_TVOUT2_MEM_BASE,   0x1000 },
        { "tvout3",     IT1G_TVOUT3_MEM_BASE,   0x1000 },
        { "mpvd",       IT1G_MPVD_MEM_BASE,     0x70000 },
        { "h264bpd",    IT1G_H264BPD_MEM_BASE,  0x1000 },
        { "dmac1",      IT1G_DMAC1_MEM_BASE,    0x1000 },
        { "mbx",        IT1G_MBX_MEM_BASE,      0x1000000 },
        { "spi0",       IT1G_SPI0_MEM_BASE,     0x1000 },
        { "usb-phys",   IT1G_USBPHYS_MEM_BASE,  0x1000 },
        { "iis2",       IT1G_IIS2_MEM_BASE,     0x1000 },
        { "iis1",       IT1G_IIS1_MEM_BASE,     0x1000 },
        { "spi1",       IT1G_SPI1_MEM_BASE,     0x1000 },
        { "spi2",       IT1G_SPI2_MEM_BASE,     0x1000 },
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
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(busdev, &error_fatal);
    sysbus_mmio_map(busdev, 0, IT1G_TIMER1_MEM_BASE);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8900_TIMER1_IRQ));
    nms->timer1 = S5L_TIMER(dev);

    dev = qdev_new(TYPE_S5L_SYSIC);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(busdev, &error_fatal);
    sysbus_mmio_map(busdev, 0, IT1G_SYSIC_MEM_BASE);
    static const int gpio_irqs[] = {
        S5L8900_GPIO_G0_IRQ, S5L8900_GPIO_G1_IRQ, S5L8900_GPIO_G2_IRQ, S5L8900_GPIO_G3_IRQ, S5L8900_GPIO_G4_IRQ,
    };
    for (int grp = 0; grp < ARRAY_SIZE(gpio_irqs); grp++) {
        sysbus_connect_irq(busdev, grp, s5l8900_get_irq(nms, gpio_irqs[grp]));
    }

    dev = qdev_new(TYPE_S5L_GPIO);
    nms->gpio_state = S5L_GPIO(dev);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(sysmem, IT1G_GPIO_MEM_BASE, &nms->gpio_state->iomem);

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

    // the I2C buses: the accelerometer on I2C0, the PMU on I2C1
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
    i2c_slave_create_simple(i2c[1]->bus, "pcf50633", 0x73);

    qemu_register_reset(it1g_cpu_reset, nms);
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

static void it1g_instance_init(Object *obj)
{
    object_property_add_str(obj, "nor", it1g_get_nor, it1g_set_nor);
    object_property_set_description(obj, "nor", "Path to the S5L8900 NOR image");
    object_property_add_str(obj, "boot-image", it1g_get_boot_image, it1g_set_boot_image);
    object_property_set_description(obj, "boot-image", "Img2 image (or its 8900 container) to start instead of LLB "
                                    "from the NOR");
    object_property_add_str(obj, "bootrom", it1g_get_bootrom, it1g_set_bootrom);
    object_property_set_description(obj, "bootrom", "Not supported on the iPod touch 1G yet");
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
