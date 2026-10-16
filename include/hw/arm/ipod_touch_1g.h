#ifndef HW_ARM_IPOD_TOUCH_1G_H
#define HW_ARM_IPOD_TOUCH_1G_H

#include "qemu/osdep.h"
#include "exec/hwaddr.h"
#include "hw/core/boards.h"
#include "hw/intc/pl192.h"
#include "cpu.h"
#include "hw/arm/s5l_timer.h"
#include "hw/arm/s5l_gpio.h"
#include "hw/arm/s5l_sysic.h"
#include "hw/arm/s5l_spi.h"

#define TYPE_IPOD_TOUCH_1G "iPod-Touch-1G"
#define TYPE_IPOD_TOUCH_1G_MACHINE MACHINE_TYPE_NAME(TYPE_IPOD_TOUCH_1G)
#define IPOD_TOUCH_1G_MACHINE(obj) \
    OBJECT_CHECK(IPodTouch1GMachineState, (obj), TYPE_IPOD_TOUCH_1G_MACHINE)

// The S5L8900 of the iPod touch (1st generation), n45ap
#define S5L8900_VIC_SIZE        32

#define S5L8900_GPIO_G6_IRQ     0x00
#define S5L8900_GPIO_G5_IRQ     0x01
#define S5L8900_GPIO_G4_IRQ     0x02
#define S5L8900_GPIO_G3_IRQ     0x03
#define S5L8900_TIMER1_IRQ      0x07
#define S5L8900_SPI0_IRQ        0x09
#define S5L8900_SPI1_IRQ        0x0A
#define S5L8900_SPI2_IRQ        0x0B
#define S5L8900_MBX_IRQ         0x0C
#define S5L8900_LCD_IRQ         0x0D
#define S5L8900_DMAC0_IRQ       0x10
#define S5L8900_DMAC1_IRQ       0x11
#define S5L8900_I2C0_IRQ        0x15
#define S5L8900_I2C1_IRQ        0x16
#define S5L8900_UART0_IRQ       0x18
#define S5L8900_TVOUT_SDO_IRQ   0x1E
#define S5L8900_GPIO_G2_IRQ     0x1F
#define S5L8900_GPIO_G1_IRQ     0x20
#define S5L8900_GPIO_G0_IRQ     0x21
#define S5L8900_ADM_IRQ         0x25
#define S5L8900_TVOUT_MIXER_IRQ 0x26
#define S5L8900_NAND_ECC_IRQ    0x2B

#define IT1G_RAM_MEM_BASE        0x08000000
#define IT1G_RAM_SIZE            0x08000000    // 128 MB
#define IT1G_FRAMEBUFFER_BASE    0x0fe00000
#define IT1G_IBOOT_RAM_BASE      0x18000000    // where LLB loads iBoot; LLB's page table maps 2 MB
#define IT1G_IBOOT_RAM_SIZE      0x200000
#define IT1G_VROM_MEM_BASE       0x20000000
#define IT1G_SRAM_MEM_BASE       0x22000000
#define IT1G_SRAM_SIZE           0x40000
#define IT1G_NOR_MEM_BASE        0x24000000
#define IT1G_NOR_SIZE            0x100000      // 1 MB

#define IT1G_SHA1_MEM_BASE       0x38000000
#define IT1G_CLOCK0_MEM_BASE     0x38100000
#define IT1G_DMAC0_MEM_BASE      0x38200000
#define IT1G_USBOTG_MEM_BASE     0x38400000
#define IT1G_ADM_MEM_BASE        0x38800000
#define IT1G_DISPLAY_MEM_BASE    0x38900000
#define IT1G_NAND_MEM_BASE       0x38A00000
#define IT1G_AES_MEM_BASE        0x38C00000
#define IT1G_SDIO_MEM_BASE       0x38D00000
#define IT1G_VIC0_MEM_BASE       0x38E00000
#define IT1G_VIC1_MEM_BASE       0x38E01000
#define IT1G_EDGEIC_MEM_BASE     0x38E02000
#define IT1G_NAND_ECC_MEM_BASE   0x38F00000
#define IT1G_TVOUT1_MEM_BASE     0x39100000
#define IT1G_TVOUT2_MEM_BASE     0x39200000
#define IT1G_TVOUT3_MEM_BASE     0x39300000
#define IT1G_MPVD_MEM_BASE       0x39600000
#define IT1G_H264BPD_MEM_BASE    0x39800000
#define IT1G_DMAC1_MEM_BASE      0x39900000
#define IT1G_SYSIC_MEM_BASE      0x39A00000
#define IT1G_MBX_MEM_BASE        0x3B000000
#define IT1G_SPI0_MEM_BASE       0x3C300000
#define IT1G_USBPHYS_MEM_BASE    0x3C400000
#define IT1G_CLOCK1_MEM_BASE     0x3C500000
#define IT1G_I2C0_MEM_BASE       0x3C600000
#define IT1G_I2C1_MEM_BASE       0x3C900000
#define IT1G_IIS2_MEM_BASE       0x3CA00000
#define IT1G_UART0_MEM_BASE      0x3CC00000
#define IT1G_IIS1_MEM_BASE       0x3CD00000
#define IT1G_SPI1_MEM_BASE       0x3CE00000
#define IT1G_SPI2_MEM_BASE       0x3D200000
#define IT1G_IIS0_MEM_BASE       0x3D400000
#define IT1G_TIMER1_MEM_BASE     0x3E200000
#define IT1G_WATCHDOG_MEM_BASE   0x3E300000
#define IT1G_GPIO_MEM_BASE       0x3E400000
#define IT1G_CHIPID_MEM_BASE     0x3E500000

#define IT1G_GPIO_FSEL           0x320         // the pin configuration register (0x1e0 on the S5L8720)
#define IT1G_GPIO_LCD_CS         0x1800        // the LCD panel's chip select, on SPI1
// the buttons (the device tree's buttons node: function-button_hold and _menu, read high while pressed) and their
// GPIO interrupts
#define IT1G_GPIO_BUTTON_POWER   0x1605
#define IT1G_GPIO_BUTTON_HOME    0x1606
#define IT1G_GPIO_BUTTON_POWER_IRQ 0x2D
#define IT1G_GPIO_BUTTON_HOME_IRQ  0x2E
#define IT1G_GPIO_LIGHTSENSOR_IRQ  0x4C        // the device tree's als node

#define IT1G_UART_COUNT          5
#define IT1G_UART_STRIDE         0x4000

typedef struct {
    MachineClass parent;
} IPodTouch1GMachineClass;

typedef struct {
    MachineState parent;
    ARMCPU *cpu;
    qemu_irq **irq;
    PL192State *vic0;
    PL192State *vic1;
    S5LTimerState *timer1;
    S5LGPIOState *gpio_state;
    DeviceState *sysic;
    S5LSPIState *spi[3];
    struct PFlashCFI02 *nor;
    uint32_t periph_port_remap;
    char *nor_path;
    char *boot_image_path;
    char *bootrom_path;
    char *nand_disk;
    char *nand_overlay;
    char *nand_epoch;
} IPodTouch1GMachineState;

#endif
