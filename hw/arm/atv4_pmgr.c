#include "hw/arm/atv4_pmgr.h"

static uint64_t atv4_pmgr_pll_read(void *opaque, hwaddr addr, unsigned size)
{
    //fprintf(stderr, "%s: offset = 0x%08x\n", __func__, addr);
    ATV4PMGRState *s = (struct ATV4PMGRState *) opaque;

    uint8_t pll_index = addr / 0x1000;
    if(pll_index >= 6) {
        hw_error("%s: Trying to access non-existant PLL %d\n", __func__, pll_index);
    }

    if(addr % 0x1000 == rPMGR_PLL_EXT_BYPASS_CTL) {
        return s->pll_bypass_cfg[pll_index];
    }

    return 0;
}

static void atv4_pmgr_pll_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    //fprintf(stderr, "%s: writing 0x%08x to 0x%08x\n", __func__, value, addr);
    ATV4PMGRState *s = (struct ATV4PMGRState *) opaque;

    uint8_t pll_index = addr / 0x1000;
    if(pll_index >= 6) {
        hw_error("%s: Trying to access non-existant PLL %d\n", __func__, pll_index);
    }

    if(addr % 0x1000 == rPMGR_PLL_EXT_BYPASS_CTL) {
        if(value & PMGR_PLL_EXT_BYPASS) {
            printf("Enabling PLL %d bypass\n", pll_index);
            s->pll_bypass_cfg[pll_index] |= PMGR_PLL_BYP_ENABLED;
        }
        else {
            printf("Disabling PLL %d bypass\n", pll_index);
            s->pll_bypass_cfg[pll_index] &= ~PMGR_PLL_BYP_ENABLED;
        }
    }
}

static uint64_t atv4_pmgr_volman_read(void *opaque, hwaddr addr, unsigned size)
{
    //fprintf(stderr, "%s: offset = 0x%08x\n", __func__, addr);

    switch (addr) {
        case rPMGR_VOLMAN_CTL:
            // TODO
            break;
        default:
            hw_error("%s: reading from unknown PMGR volman register 0x%08x\n", __func__, addr);
    }

    return 0;
}

static void atv4_pmgr_volman_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    //fprintf(stderr, "%s: writing 0x%08x to 0x%08x\n", __func__, value, addr);
    ATV4PMGRState *s = (struct ATV4PMGRState *) opaque;

    switch(addr) {
      default:
        break;
    }
}

static uint64_t atv4_pmgr_ps_read(void *opaque, hwaddr addr, unsigned size)
{
    //fprintf(stderr, "%s: offset = 0x%08x\n", __func__, addr);

    switch (addr) {
        case rPMGR_CPU0_PS ... rPMGR_VENC_ME1_PS:
            return 0xFF; // turned on
            break;
        default:
            hw_error("%s: reading from unknown PMGR PS register 0x%08x\n", __func__, addr);
    }

    return 0;
}

static void atv4_pmgr_ps_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    fprintf(stderr, "%s: writing 0x%08x to 0x%08x\n", __func__, value, addr);
    ATV4PMGRState *s = (struct ATV4PMGRState *) opaque;

    switch(addr) {
      default:
        break;
    }
}

static uint64_t atv4_pmgr_gfx_read(void *opaque, hwaddr addr, unsigned size)
{
    //fprintf(stderr, "%s: offset = 0x%08x\n", __func__, addr);

    switch (addr) {
        case rPMGR_GFX_PERF_STATE_CTL:
            // TODO return the right GFX state
            break;
        default:
            hw_error("%s: reading from unknown PMGR GFX register 0x%08x\n", __func__, addr);
    }

    return 0;
}

static void atv4_pmgr_gfx_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    //fprintf(stderr, "%s: writing 0x%08x to 0x%08x\n", __func__, value, addr);
    ATV4PMGRState *s = (struct ATV4PMGRState *) opaque;

    switch(addr) {
      default:
        break;
    }
}

static uint64_t atv4_pmgr_acg_read(void *opaque, hwaddr addr, unsigned size)
{
    //fprintf(stderr, "%s: offset = 0x%08x\n", __func__, addr);

    switch (addr) {
        case rPMGR_MISC_ACG:
            // TODO return the right ACG state
            break;
        default:
            hw_error("%s: reading from unknown PMGR ACG register 0x%08x\n", __func__, addr);
    }

    return 0;
}

static void atv4_pmgr_acg_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    //fprintf(stderr, "%s: writing 0x%08x to 0x%08x\n", __func__, value, addr);
    ATV4PMGRState *s = (struct ATV4PMGRState *) opaque;

    switch(addr) {
      default:
        break;
    }
}

static uint64_t atv4_pmgr_clk_read(void *opaque, hwaddr addr, unsigned size)
{
    //fprintf(stderr, "%s: offset = 0x%08x\n", __func__, addr);

    switch (addr) {
        case rPMGR_CLK_DIVIDER_ACG_CFG:
            // TODO return the right CLK state
            break;
        default:
            hw_error("%s: reading from unknown PMGR CLK register 0x%08x\n", __func__, addr);
    }

    return 0;
}

static void atv4_pmgr_clk_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    //fprintf(stderr, "%s: writing 0x%08x to 0x%08x\n", __func__, value, addr);
    ATV4PMGRState *s = (struct ATV4PMGRState *) opaque;

    switch(addr) {
      default:
        break;
    }
}

static uint64_t atv4_pmgr_clkcfg_read(void *opaque, hwaddr addr, unsigned size)
{
    //fprintf(stderr, "%s: offset = 0x%08x\n", __func__, addr);

    switch (addr) {
        case rPMGR_MCU_FIXED_CLK_CFG ... rPMGR_ISP_REF1_CLK_CFG:
            // TODO return the right CLK configuration
            break;
        default:
            hw_error("%s: reading from unknown PMGR CLK CFG register 0x%08x\n", __func__, addr);
    }

    return 0;
}

static void atv4_pmgr_clkcfg_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    //fprintf(stderr, "%s: writing 0x%08x to 0x%08x\n", __func__, value, addr);
    ATV4PMGRState *s = (struct ATV4PMGRState *) opaque;

    switch(addr) {
      default:
        break;
    }
}

static uint64_t atv4_pmgr_soc_read(void *opaque, hwaddr addr, unsigned size)
{
    //fprintf(stderr, "%s: offset = 0x%08x\n", __func__, addr);

    switch (addr) {
        case rPMGR_SOC_PERF_STATE_CTL:
            // TODO return the right SOC configuration
            break;
        default:
            hw_error("%s: reading from unknown PMGR SOC state register 0x%08x\n", __func__, addr);
    }

    return 0;
}

static void atv4_pmgr_soc_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    //fprintf(stderr, "%s: writing 0x%08x to 0x%08x\n", __func__, value, addr);
    ATV4PMGRState *s = (struct ATV4PMGRState *) opaque;

    switch(addr) {
      default:
        break;
    }
}

static uint64_t atv4_pmgr_scratch_read(void *opaque, hwaddr addr, unsigned size)
{
    //fprintf(stderr, "%s: offset = 0x%08x\n", __func__, addr);

    switch (addr) {
        case 0x0 ... 0x60:
            // TODO return the right scratch configuration
            break;
        default:
            hw_error("%s: reading from unknown PMGR SCRATCH register 0x%08x\n", __func__, addr);
    }

    return 0;
}

static void atv4_pmgr_scratch_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    //fprintf(stderr, "%s: writing 0x%08x to 0x%08x\n", __func__, value, addr);
    ATV4PMGRState *s = (struct ATV4PMGRState *) opaque;

    switch(addr) {
      default:
        break;
    }
}

static uint64_t atv4_pmgr_pwrgate_read(void *opaque, hwaddr addr, unsigned size)
{
    //fprintf(stderr, "%s: offset = 0x%08x\n", __func__, addr);

    switch (addr) {
        case 0x0 ... 0x300:
            // TODO return the right scratch configuration
            break;
        default:
            hw_error("%s: reading from unknown PMGR PWRGATE register 0x%08x\n", __func__, addr);
    }

    return 0;
}

static void atv4_pmgr_pwrgate_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    //fprintf(stderr, "%s: writing 0x%08x to 0x%08x\n", __func__, value, addr);
    ATV4PMGRState *s = (struct ATV4PMGRState *) opaque;

    switch(addr) {
      default:
        break;
    }
}

static const MemoryRegionOps atv4_pmgr_pll_ops = {
    .read = atv4_pmgr_pll_read,
    .write = atv4_pmgr_pll_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps atv4_pmgr_volman_ops = {
    .read = atv4_pmgr_volman_read,
    .write = atv4_pmgr_volman_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps atv4_pmgr_ps_ops = {
    .read = atv4_pmgr_ps_read,
    .write = atv4_pmgr_ps_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps atv4_pmgr_gfx_ops = {
    .read = atv4_pmgr_gfx_read,
    .write = atv4_pmgr_gfx_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps atv4_pmgr_acg_ops = {
    .read = atv4_pmgr_acg_read,
    .write = atv4_pmgr_acg_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps atv4_pmgr_clk_ops = {
    .read = atv4_pmgr_clk_read,
    .write = atv4_pmgr_clk_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps atv4_pmgr_clkcfg_ops = {
    .read = atv4_pmgr_clkcfg_read,
    .write = atv4_pmgr_clkcfg_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps atv4_pmgr_soc_ops = {
    .read = atv4_pmgr_soc_read,
    .write = atv4_pmgr_soc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps atv4_pmgr_scratch_ops = {
    .read = atv4_pmgr_scratch_read,
    .write = atv4_pmgr_scratch_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps atv4_pmgr_pwrgate_ops = {
    .read = atv4_pmgr_pwrgate_read,
    .write = atv4_pmgr_pwrgate_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void atv4_pmgr_init(Object *obj)
{
    ATV4PMGRState *s = ATV4_PMGR(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    // PLLs
    memory_region_init_io(&s->pll_iomem, obj, &atv4_pmgr_pll_ops, s, TYPE_ATV4_PMGR_PLL, 0x10000);
    sysbus_init_mmio(sbd, &s->pll_iomem);

    // voltage manager
    memory_region_init_io(&s->volman_iomem, obj, &atv4_pmgr_volman_ops, s, TYPE_ATV4_PMGR_VOLMAN, 0x4);
    sysbus_init_mmio(sbd, &s->volman_iomem);

    // power states
    memory_region_init_io(&s->ps_iomem, obj, &atv4_pmgr_ps_ops, s, TYPE_ATV4_PMGR_PS, 0x1010);
    sysbus_init_mmio(sbd, &s->ps_iomem);

    // graphics subsystem
    memory_region_init_io(&s->gfx_iomem, obj, &atv4_pmgr_gfx_ops, s, TYPE_ATV4_PMGR_GFX, 0x4);
    sysbus_init_mmio(sbd, &s->gfx_iomem);

    // automatic clock gating
    memory_region_init_io(&s->acg_iomem, obj, &atv4_pmgr_acg_ops, s, TYPE_ATV4_PMGR_ACG, 0x4);
    sysbus_init_mmio(sbd, &s->acg_iomem);

    // clock
    memory_region_init_io(&s->clk_iomem, obj, &atv4_pmgr_clk_ops, s, TYPE_ATV4_PMGR_CLK, 0x4);
    sysbus_init_mmio(sbd, &s->clk_iomem);

    // clock configurations
    memory_region_init_io(&s->clkcfg_iomem, obj, &atv4_pmgr_clkcfg_ops, s, TYPE_ATV4_PMGR_CLKCFG, 0x6008);
    sysbus_init_mmio(sbd, &s->clkcfg_iomem);

    // SOC performance
    memory_region_init_io(&s->soc_iomem, obj, &atv4_pmgr_soc_ops, s, TYPE_ATV4_PMGR_SOC, 0xFF);
    sysbus_init_mmio(sbd, &s->soc_iomem);

    // scratch
    memory_region_init_io(&s->scratch_iomem, obj, &atv4_pmgr_scratch_ops, s, TYPE_ATV4_PMGR_SCRATCH, 0x60);
    sysbus_init_mmio(sbd, &s->scratch_iomem);

    // power gate
    memory_region_init_io(&s->pwrgate_iomem, obj, &atv4_pmgr_pwrgate_ops, s, TYPE_ATV4_PMGR_PWRGATE, 0x300);
    sysbus_init_mmio(sbd, &s->pwrgate_iomem);
}

static void atv4_pmgr_class_init(ObjectClass *klass, void *data)
{
    
}

static const TypeInfo atv4_pmgr_type_info = {
    .name = TYPE_ATV4_PMGR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ATV4PMGRState),
    .instance_init = atv4_pmgr_init,
    .class_init = atv4_pmgr_class_init,
};

static void atv4_pmgr_register_types(void)
{
    type_register_static(&atv4_pmgr_type_info);
}

type_init(atv4_pmgr_register_types)