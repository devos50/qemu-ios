#ifndef HW_ARM_ATV4_H
#define HW_ARM_ATV4_H

#include "exec/hwaddr.h"
#include "hw/boards.h"
#include "cpu.h"
#include "hw/intc/apple_aic.h"
#include "hw/arm/boot.h"
#include "hw/arm/atv4_chipid.h"
#include "hw/arm/atv4_pmgr.h"
#include "hw/arm/atv4_gpio.h"
#include "hw/arm/atv4_ausb.h"
#include "hw/arm/atv4_ausb_widget.h"
#include "hw/arm/atv4_spi.h"
#include "hw/arm/atv4_anc.h"

#define TYPE_ATV4 "ATV4"

#define ATV4_CPREG_VAR_NAME(name) cpreg_##name
#define ATV4_CPREG_VAR_DEF(name) uint64_t ATV4_CPREG_VAR_NAME(name)

#define TYPE_ATV4_MACHINE   MACHINE_TYPE_NAME(TYPE_ATV4)
#define ATV4_MACHINE(obj) \
    OBJECT_CHECK(ATV4MachineState, (obj), TYPE_ATV4_MACHINE)

#define NUM_SPI_PORTS 4
#define SRAM_LENGTH 0x400000  // 4MB

// memory addresses
#define VROM_MEM_BASE         0x100000000
#define SRAM_MEM_BASE         0x180000000
#define UNKNOWN2_MEM_BASE     0x202220000
#define UNKNOWN3_MEM_BASE     0x202240000
#define PPNNPL_MEM_BASE       0x208081000
#define ANC0_MEM_BASE         0x208800000
#define ANC1_MEM_BASE         0x208900000
#define SPI0_MEM_BASE         0x20a080000
#define SPI1_MEM_BASE         0x20a084000
#define SPI2_MEM_BASE         0x20a088000
#define SPI3_MEM_BASE         0x20a08C000
#define AUSB_MEM_BASE         0x20c000000
#define AUSB_WIDGET_MEM_BASE  0x20c900000
#define PMGR_MEM_BASE         0x20e000000
#define AIC_MEM_BASE          0x20e100000
#define GPIO_MEM_BASE         0x20e300000
#define PMGR_PLL_MEM_BASE     (PMGR_MEM_BASE)
#define PMGR_CLKCFG_MEM_BASE  (PMGR_MEM_BASE + 0x10000)
#define PMGR_CLK_MEM_BASE     (PMGR_MEM_BASE + 0x10308)
#define PMGR_SOC_MEM_BASE     (PMGR_MEM_BASE + 0x1a000)
#define PMGR_GFX_MEM_BASE     (PMGR_MEM_BASE + 0x1c200)
#define PMGR_PS_MEM_BASE      (PMGR_MEM_BASE + 0x20000)
#define PMGR_PWRGATE_MEM_BASE (PMGR_MEM_BASE + 0x22000)
#define PMGR_VOLMAN_MEM_BASE  (PMGR_MEM_BASE + 0x23000)
#define PMGR_SCRATCH_MEM_BASE (PMGR_MEM_BASE + 0x29000)
#define PMGR_ACG_MEM_BASE     (PMGR_MEM_BASE + 0x32000)
#define CHIPID_MEM_BASE       0x20e02a000

typedef struct {
    MachineClass parent;
} ATV4MachineClass;

typedef struct {
	MachineState parent;
	AddressSpace *nsas;
	qemu_irq **irq;
	ARMCPU *cpu;
	AppleAICState *aic_state;
	ATV4ChipIDState *chipid_state;
	ATV4PMGRState *pmgr_state;
	ATV4GPIOState *gpio_state;
	ATV4AUSBState *ausb_state;
	ATV4AUSBWidgetState *ausb_widget_state;
	ATV4SPIState *spi_state[4];
	ATV4ANCState *anc0_state;
	ATV4ANCState *anc1_state;
	ATV4_CPREG_VAR_DEF(REG0);
	ATV4_CPREG_VAR_DEF(REG1);
	ATV4_CPREG_VAR_DEF(REG2);
	ATV4_CPREG_VAR_DEF(REG3);
	ATV4_CPREG_VAR_DEF(REG4);
} ATV4MachineState;

#endif