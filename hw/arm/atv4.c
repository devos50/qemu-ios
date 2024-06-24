#include "qemu/osdep.h"
#include "qapi/error.h"
#include "exec/address-spaces.h"
#include "sysemu/reset.h"
#include "target/arm/cpu.h"
#include "target/arm/cpregs.h"
#include "hw/arm/atv4.h"

#define VMSTATE_ATV4_CPREG(name) \
        VMSTATE_UINT64(ATV4_CPREG_VAR_NAME(name), ATV4MachineState)

#define ATV4_CPREG_DEF(p_name, p_op0, p_op1, p_crn, p_crm, p_op2, p_access, p_reset) \
    {                                                                              \
        .cp = CP_REG_ARM64_SYSREG_CP,                                              \
        .name = #p_name, .opc0 = p_op0, .crn = p_crn, .crm = p_crm,                \
        .opc1 = p_op1, .opc2 = p_op2, .access = p_access, .resetvalue = p_reset,   \
        .state = ARM_CP_STATE_AA64, .type = ARM_CP_OVERRIDE,                       \
        .fieldoffset = 5                                  \
    }

static void allocate_ram(MemoryRegion *top, const char *name, uint64_t addr, uint32_t size)
{
    MemoryRegion *sec = g_new(MemoryRegion, 1);
    memory_region_init_ram(sec, NULL, name, size, &error_fatal);
    memory_region_add_subregion(top, addr, sec);
}

static const ARMCPRegInfo atv4_cp_reginfo_tcg[] = {
    ATV4_CPREG_DEF(REG0, 3, 0, 15, 1, 0, PL1_RW, 0),
    ATV4_CPREG_DEF(REG1, 3, 0, 15, 5, 0, PL1_RW, 0),
    ATV4_CPREG_DEF(REG2, 3, 6, 12, 0, 0, PL1_RW, 0),
    ATV4_CPREG_DEF(REG3, 3, 6, 1, 1, 0, PL1_RW, 0),
    ATV4_CPREG_DEF(REG4, 3, 6, 1, 0, 0, PL1_RW, 0),
};

static void atv4_cpu_setup(MachineState *machine, MemoryRegion **sysmem, ARMCPU **cpu, AddressSpace **nsas)
{
    Object *cpuobj = object_new(machine->cpu_type);
    *cpu = ARM_CPU(cpuobj);
    CPUState *cs = CPU(*cpu);

    *sysmem = get_system_memory();

    object_property_set_link(cpuobj, "memory", OBJECT(*sysmem), &error_abort);

    object_property_set_bool(cpuobj, "realized", true, &error_fatal);

    *nsas = cpu_get_address_space(cs, ARMASIdx_NS);

    define_arm_cp_regs(*cpu, atv4_cp_reginfo_tcg);

    object_unref(cpuobj);
}

static void atv4_cpu_reset(void *opaque)
{
    ATV4MachineState *nms = ATV4_MACHINE((MachineState *)opaque);
    ARMCPU *cpu = nms->cpu;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);

    cpu_set_pc(CPU(cpu), VROM_MEM_BASE);
}

static void atv4_memory_setup(MachineState *machine, MemoryRegion *sysmem, AddressSpace *nsas)
{
    // ATV4MachineState *nms = ATV4_MACHINE(machine);
    allocate_ram(sysmem, "unknown1", UNKNOWN1_MEM_BASE, 0x200000);
    allocate_ram(sysmem, "unknown2", UNKNOWN2_MEM_BASE, 0x1000);
    allocate_ram(sysmem, "unknown3", UNKNOWN3_MEM_BASE, 0x20);

    // load the bootrom (vrom)
    uint8_t *file_data = NULL;
    unsigned long fsize;
    if (g_file_get_contents("/Users/martijndevos/Downloads/atv4_securerom", (char **)&file_data, &fsize, NULL)) {
        allocate_ram(sysmem, "vrom", VROM_MEM_BASE, 0x200000);
        address_space_rw(nsas, VROM_MEM_BASE, MEMTXATTRS_UNSPECIFIED, (uint8_t *)file_data, fsize, 1);
    }
}

static void atv4_machine_init(MachineState *machine)
{
    ATV4MachineState *nms = ATV4_MACHINE(machine);
    MemoryRegion *sysmem;
    AddressSpace *nsas;
    ARMCPU *cpu;

    atv4_cpu_setup(machine, &sysmem, &cpu, &nsas);

    nms->cpu = cpu;
    nms->nsas = nsas;

    atv4_memory_setup(machine, sysmem, nsas);

    // init the interrupt controller
    AppleAICState *aic_state = apple_aic_create();
    nms->aic_state = aic_state;
    SysBusDevice *busdev = SYS_BUS_DEVICE(nms->aic_state);
    sysbus_realize(busdev, &error_fatal);
    for(int i = 0; i < nms->aic_state->numCPU; i++) {
        memory_region_add_subregion(sysmem, AIC_MEM_BASE + i * 0x1000, &aic_state->cpus[i].iomem);
    }

    // init the chip ID module
    DeviceState *dev = qdev_new("atv4.chipid");
    ATV4ChipIDState *chipid_state = ATV4_CHIPID(dev);
    nms->chipid_state = chipid_state;
    memory_region_add_subregion(sysmem, CHIPID_MEM_BASE, &chipid_state->iomem);

    // init the PMGR module
    dev = qdev_new("atv4.pmgr");
    ATV4PMGRState *pmgr_state = ATV4_PMGR(dev);
    nms->pmgr_state = pmgr_state;
    memory_region_add_subregion(sysmem, PMGR_PLL_MEM_BASE, &pmgr_state->pll_iomem);
    memory_region_add_subregion(sysmem, PMGR_VOLMAN_MEM_BASE, &pmgr_state->volman_iomem);
    memory_region_add_subregion(sysmem, PMGR_PS_MEM_BASE, &pmgr_state->ps_iomem);
    memory_region_add_subregion(sysmem, PMGR_GFX_MEM_BASE, &pmgr_state->gfx_iomem);
    memory_region_add_subregion(sysmem, PMGR_ACG_MEM_BASE, &pmgr_state->acg_iomem);
    memory_region_add_subregion(sysmem, PMGR_CLK_MEM_BASE, &pmgr_state->clk_iomem);
    memory_region_add_subregion(sysmem, PMGR_CLKCFG_MEM_BASE, &pmgr_state->clkcfg_iomem);
    memory_region_add_subregion(sysmem, PMGR_SOC_MEM_BASE, &pmgr_state->soc_iomem);
    memory_region_add_subregion(sysmem, PMGR_SCRATCH_MEM_BASE, &pmgr_state->scratch_iomem);
    memory_region_add_subregion(sysmem, PMGR_PWRGATE_MEM_BASE, &pmgr_state->pwrgate_iomem);

    // init GPIO
    dev = qdev_new("atv4.gpio");
    ATV4GPIOState *gpio_state = ATV4_GPIO(dev);
    nms->gpio_state = gpio_state;
    memory_region_add_subregion(sysmem, GPIO_MEM_BASE, &gpio_state->iomem);

    // init the Apple USB
    dev = qdev_new("atv4.ausb");
    ATV4AUSBState *ausb_state = ATV4_AUSB(dev);
    nms->ausb_state = ausb_state;
    memory_region_add_subregion(sysmem, AUSB_MEM_BASE, &ausb_state->iomem);

    qemu_register_reset(atv4_cpu_reset, nms);
}

static void atv4_instance_init(Object *obj)
{
    // TODO
}

static void atv4_machine_class_init(ObjectClass *klass, void *data)
{
    MachineClass *mc = MACHINE_CLASS(klass);

    mc->desc = "ATV4";
    mc->init = atv4_machine_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("max");
}

static const TypeInfo atv4_machine_info = {
    .name          = TYPE_ATV4_MACHINE,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(ATV4MachineState),
    .class_size    = sizeof(ATV4MachineClass),
    .class_init    = atv4_machine_class_init,
    .instance_init = atv4_instance_init,
};

static void atv4_machine_types(void)
{
    type_register_static(&atv4_machine_info);
}

type_init(atv4_machine_types)