#ifndef HW_ARM_ATV4_PMGR_H
#define HW_ARM_ATV4_PMGR_H

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/sysbus.h"

enum boot_device {
        BOOT_DEVICE_NOR = 0,
        BOOT_DEVICE_SPI,
        BOOT_DEVICE_NAND,
        BOOT_DEVICE_NVME,
        BOOT_DEVICE_USBDFU,
        BOOT_DEVICE_TBTDFU,
        BOOT_DEVICE_XMODEM,
};

#define TYPE_ATV4_PMGR            "atv4.pmgr"
#define TYPE_ATV4_PMGR_VOLMAN     "atv4.pmgr.volman"
#define TYPE_ATV4_PMGR_PS         "atv4.pmgr.ps"
#define TYPE_ATV4_PMGR_GFX        "atv4.pmgr.gfx"
#define TYPE_ATV4_PMGR_ACG        "atv4.pmgr.acg"
#define TYPE_ATV4_PMGR_CLK        "atv4.pmgr.clk"
#define TYPE_ATV4_PMGR_CLKCFG     "atv4.pmgr.clkcfg"
#define TYPE_ATV4_PMGR_SOC        "atv4.pmgr.soc"
#define TYPE_ATV4_PMGR_PLL        "atv4.pmgr.pll"
#define TYPE_ATV4_PMGR_SCRATCH    "atv4.pmgr.scratch"
#define TYPE_ATV4_PMGR_PWRGATE    "atv4.pmgr.pwrgate"
OBJECT_DECLARE_SIMPLE_TYPE(ATV4PMGRState, ATV4_PMGR)

#define NUM_PLLS 0x6

#define rPMGR_CPU0_PS           0x00000
#define rPMGR_CPU1_PS           0x00008
#define rPMGR_CPM_PS            0x00040
#define rPMGR_LIO_PS            0x00100
#define rPMGR_IOMUX_PS          0x00108
#define rPMGR_AIC_PS            0x00110
#define rPMGR_DEBUG_PS          0x00118
#define rPMGR_DWI_PS            0x00120
#define rPMGR_GPIO_PS           0x00128
#define rPMGR_MCA0_PS           0x00130
#define rPMGR_MCA1_PS           0x00138
#define rPMGR_MCA2_PS           0x00140
#define rPMGR_MCA3_PS           0x00148
#define rPMGR_MCA4_PS           0x00150
#define rPMGR_PWM0_PS           0x00158
#define rPMGR_I2C0_PS           0x00160
#define rPMGR_I2C1_PS           0x00168
#define rPMGR_I2C2_PS           0x00170
#define rPMGR_I2C3_PS           0x00178
#define rPMGR_SPI0_PS           0x00180
#define rPMGR_SPI1_PS           0x00188
#define rPMGR_SPI2_PS           0x00190
#define rPMGR_SPI3_PS           0x00198
#define rPMGR_UART0_PS          0x001a0
#define rPMGR_UART1_PS          0x001a8
#define rPMGR_UART2_PS          0x001b0
#define rPMGR_UART3_PS          0x001b8
#define rPMGR_UART4_PS          0x001c0
#define rPMGR_UART5_PS          0x001c8
#define rPMGR_UART6_PS          0x001d0
#define rPMGR_UART7_PS          0x001d8
#define rPMGR_UART8_PS          0x001e0
#define rPMGR_AES0_PS           0x001e8
#define rPMGR_SIO_PS            0x001f0
#define rPMGR_SIO_P_PS          0x001f8
#define rPMGR_HSIC0PHY_PS       0x00200
#define rPMGR_HSIC1PHY_PS       0x00208
#define rPMGR_ISPSENS0_PS       0x00210
#define rPMGR_ISPSENS1_PS       0x00218
#define rPMGR_PCIE_REF_PS       0x00220
#define rPMGR_ANP_PS            0x00228
#define rPMGR_MCC_PS            0x00230
#define rPMGR_MCU_PS            0x00238
#define rPMGR_AMP_PS            0x00240
#define rPMGR_USB_PS            0x00248
#define rPMGR_USBCTLREG_PS      0x00250
#define rPMGR_USB2HOST0_PS      0x00258
#define rPMGR_USB2HOST0_OHCI_PS 0x00260
#define rPMGR_USB2HOST1_PS      0x00268
#define rPMGR_USB2HOST1_OHCI_PS 0x00270
#define rPMGR_USB2HOST2_PS      0x00278
#define rPMGR_USB2HOST2_OHCI_PS 0x00280
#define rPMGR_USB_OTG_PS        0x00288
#define rPMGR_SMX_PS            0x00290
#define rPMGR_SF_PS             0x00298
#define rPMGR_CP_PS             0x002a0
#define rPMGR_DISP1_PS          0x002c8
#define rPMGR_ISP_PS            0x002d0
#define rPMGR_MEDIA_PS          0x002d8
#define rPMGR_MSR_PS            0x002e0
#define rPMGR_JPG_PS            0x002e8
#define rPMGR_VDEC0_PS          0x002f0
#define rPMGR_VENC_CPU_PS       0x00300
#define rPMGR_PCIE_PS           0x00308
#define rPMGR_PCIE_AUX_PS       0x00310
#define rPMGR_ANS_PS            0x00318
#define rPMGR_GFX_PS            0x00320
#define rPMGR_SEP_PS            0x00400
#define rPMGR_VENC_PIPE_PS      0x01000
#define rPMGR_VENC_ME0_PS       0x01008
#define rPMGR_VENC_ME1_PS       0x01010

#define rPMGR_MCU_FIXED_CLK_CFG        0x00000
#define rPMGR_MCU_CLK_CFG              0x00004
#define rPMGR_MIPI_DSI_CLK_CFG         0x00008
#define rPMGR_NCO_REF0_CLK_CFG         0x0000c
#define rPMGR_NCO_REF1_CLK_CFG         0x00010
#define rPMGR_NCO_ALG0_CLK_CFG         0x00014
#define rPMGR_NCO_ALG1_CLK_CFG         0x00018
#define rPMGR_HSICPHY_REF_12M_CLK_CFG  0x0001c
#define rPMGR_USB480_0_CLK_CFG         0x00020
#define rPMGR_USB480_1_CLK_CFG         0x00024
#define rPMGR_USB_OHCI_48M_CLK_CFG     0x00028
#define rPMGR_USB_CLK_CFG              0x0002c
#define rPMGR_USB_FREE_60M_CLK_CFG     0x00030
#define rPMGR_SIO_C_CLK_CFG            0x00034
#define rPMGR_SIO_P_CLK_CFG            0x00038
#define rPMGR_ISP_C_CLK_CFG            0x0003c
#define rPMGR_ISP_CLK_CFG              0x00040
#define rPMGR_ISP_SENSOR0_REF_CLK_CFG  0x00044
#define rPMGR_ISP_SENSOR1_REF_CLK_CFG  0x00048
#define rPMGR_VDEC_CLK_CFG             0x0004c
#define rPMGR_VENC_CLK_CFG             0x00050
#define rPMGR_VID0_CLK_CFG             0x00054
#define rPMGR_DISP0_CLK_CFG            0x00058
#define rPMGR_DISP1_CLK_CFG            0x0005c
#define rPMGR_AJPEG_IP_CLK_CFG         0x00060
#define rPMGR_AJPEG_WRAP_CLK_CFG       0x00064
#define rPMGR_MSR_CLK_CFG              0x00068
#define rPMGR_AF_CLK_CFG               0x0006c
#define rPMGR_LIO_CLK_CFG              0x00070
#define rPMGR_MCA0_M_CLK_CFG           0x00074
#define rPMGR_MCA1_M_CLK_CFG           0x00078
#define rPMGR_MCA2_M_CLK_CFG           0x0007c
#define rPMGR_MCA3_M_CLK_CFG           0x00080
#define rPMGR_MCA4_M_CLK_CFG           0x00084
#define rPMGR_SEP_CLK_CFG              0x00088
#define rPMGR_GPIO_CLK_CFG             0x0008c
#define rPMGR_SPI0_N_CLK_CFG           0x00090
#define rPMGR_SPI1_N_CLK_CFG           0x00094
#define rPMGR_SPI2_N_CLK_CFG           0x00098
#define rPMGR_SPI3_N_CLK_CFG           0x0009c
#define rPMGR_DEBUG_CLK_CFG            0x000a0
#define rPMGR_PCIE_REF_CLK_CFG         0x000a4
#define rPMGR_PCIE_APP_CLK_CFG         0x000a8
#define rPMGR_TMPS_CLK_CFG             0x000ac
#define rPMGR_MEDIA_AF_CLK_CFG         0x000b0
#define rPMGR_ISP_AF_CLK_CFG           0x000b4
#define rPMGR_GFX_AF_CLK_CFG           0x000b8
#define rPMGR_ANS_C_CLK_CFG            0x000bc
#define rPMGR_ANC_LINK_CLK_CFG         0x000c0
#define rPMGR_S0_CLK_CFG               0x10208
#define rPMGR_S1_CLK_CFG               0x1020c
#define rPMGR_S2_CLK_CFG               0x10210
#define rPMGR_S3_CLK_CFG               0x10214
#define rPMGR_ISP_REF0_CLK_CFG         0x10218
#define rPMGR_ISP_REF1_CLK_CFG         0x1021c

#define rPMGR_VOLMAN_CTL        0x0
#define rPMGR_MISC_ACG          0x0
#define rPMGR_CLK_DIVIDER_ACG_CFG 0x0
#define rPMGR_GFX_PERF_STATE_CTL 0x0
#define rPMGR_SOC_PERF_STATE_CTL 0x0

#define rPMGR_PLL_EXT_BYPASS_CTL 0x4

#define PMGR_PLL_BYP_ENABLED (1 << 31)
#define PMGR_PLL_EXT_BYPASS  (1 << 0)


typedef struct ATV4PMGRState {
    SysBusDevice busdev;
    MemoryRegion pll_iomem;
    MemoryRegion volman_iomem;
    MemoryRegion ps_iomem;
    MemoryRegion gfx_iomem;
    MemoryRegion acg_iomem;
    MemoryRegion clk_iomem;
    MemoryRegion clkcfg_iomem;
    MemoryRegion soc_iomem;
    MemoryRegion scratch_iomem;
    MemoryRegion pwrgate_iomem;
    uint64_t pll_bypass_cfg[6];
    uint64_t scratch[0x4];
} ATV4PMGRState;

#endif