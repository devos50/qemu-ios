#ifndef HW_ARM_IPOD_TOUCH_MBX_H
#define HW_ARM_IPOD_TOUCH_MBX_H

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/sysbus.h"

#define TYPE_IPOD_TOUCH_MBX "ipodtouch.mbx"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchMBXState, IPOD_TOUCH_MBX)

#define MBX1_SIZE              0x1000000
#define MBX2_SIZE              0x1000
#define MBX_NUM_REGS           (0x2000 / 4)

#define MBX_SOFT_RESET         0x080
#define MBX_CLOCK_CTRL         0x108 // bits 0-1 auto clock gating, bit 2 TA DMA
#define MBX_CORE_ID            0xf00

// Interrupts: STATUS bits are set by the hardware, by writes to STATUS itself (the driver fakes completions this way
// after a Graphics Recovery Event) and cleared through CLEAR. Bits 16-23 of STATUS count the used 2D FIFO slots.
#define MBX_INT_STATUS         0x12c
#define MBX_INT_MASK           0x130
#define MBX_INT_CLEAR          0x134
#define MBX_INT_ISP_DONE       (1 << 2)
#define MBX_INT_TSP_DONE       (1 << 3)
#define MBX_INT_TA_DONE        (1 << 4)
#define MBX_INT_TA_OUT_OF_MEM  (1 << 5)
#define MBX_INT_RENDER_DONE    (1 << 6) // also signals a finished parameter buffer setup
#define MBX_INT_REGION_DONE    (1 << 8)
#define MBX_INT_TA_ABORTED     (1 << 9)
#define MBX_INT_2D_DONE        (1 << 10)
#define MBX_INT_EVENTS         0x7ff
#define MBX_INT_3D_DONE        (MBX_INT_ISP_DONE | MBX_INT_TSP_DONE | MBX_INT_RENDER_DONE)

// Kick registers, written 1 to start an operation
#define MBX_3D_START           0x680 // render the scene or 3D blit set up in 0x600-0x6ec

// 3D render setup: a list of regions (tile x | y << 8, then the address of the region's object list), the object
// buffer, and the render target (pixel range, address, mode and stride in words)
#define MBX_3D_REGIONS         0x608
#define MBX_3D_OBJECTS         0x60c
#define MBX_3D_RT_MODE         0x650 // 1 RGB565, 2 ARGB4444, 3 ARGB1555, 6 ARGB8888
#define MBX_3D_RT_XRANGE       0x654 // x0 | x1 << 16
#define MBX_3D_RT_YRANGE       0x658
#define MBX_3D_RT_ADDR         0x65c
#define MBX_3D_RT_STRIDE       0x660
#define MBX_3D_REGION_LIST_MASK 0x1fffffc
#define MBX_3D_OBJECT_END      0xf0000000
#define MBX_3D_MAX_OBJECTS     64
#define MBX_TA_START           0x800
#define MBX_REGION_START       0x81c // -> MBX_INT_REGION_DONE
#define MBX_PB_INIT            0x838 // parameter buffer setup -> MBX_INT_RENDER_DONE

// MMU: eight directory registers hold the physical addresses of page-table pages that each map 4 MB of the 32 MB GPU
// address space with one word (a page's physical address) per 4 KB page. The kernel enables the MMU through ENABLE
// and waits for ENABLED to follow, and reloads one TLB entry (index in TLB_INDEX, page in TLB_DATA) with RELOAD,
// waiting for RELOAD_BUSY to clear.
#define MBX_MMU_DIR            0x1000
#define MBX_MMU_DIR_ENTRIES    8
#define MBX_MMU_CTRL           0x1020
#define MBX_MMU_TLB_INDEX      0x1024
#define MBX_MMU_TLB_DATA       0x1028
#define MBX_MMU_CTRL_ENABLE    (1 << 0)
#define MBX_MMU_CTRL_RELOAD    (1 << 8)
#define MBX_MMU_CTRL_ENABLED   (1 << 16)
#define MBX_MMU_CTRL_RELOAD_BUSY (1 << 24)
#define MBX_GPU_VA_SIZE        0x2000000

// Slave ports: the TA takes its vertex and state stream at 0x800000 (CPU writes or "TA DMA"), the 2D core its block
// stream at 0xa00000 (the driver writes successive words at a rolling offset within the 64 KB window).
#define MBX_TA_PORT            0x800000
#define MBX_TA_PORT_SIZE       0x200000
#define MBX_2D_PORT            0xa00000
#define MBX_2D_PORT_SIZE       0x10000

// 2D blocks, identified by the top nibble of their header word
#define MBX_2D_CLIP            0x0 // count in bits 0-7, then (x0 | x1 << 16), (y0 | y1 << 16) per rectangle
#define MBX_2D_CTRL            0x2 // bit 2: an alpha blend word follows
#define MBX_2D_SRC_OFF         0x3 // source x in bits 14-26, y in bits 0-12
#define MBX_2D_SCALE           0x6 // x scale in bits 18-27, y in bits 4-13, 5.5 fixed point
#define MBX_2D_FENCE           0x7
#define MBX_2D_BLIT            0x8 // ROP4 in bits 0-15, then a fill colour and the destination start and end
#define MBX_2D_SRC_SURF        0x9 // format and stride, then the GPU address
#define MBX_2D_DST_SURF        0xa
#define MBX_2D_FLUSH           0xf
#define MBX_2D_MAX_BLOCK       (1 + 2 * 255)

#define MBX_2D_SURF_STRIDE_MASK 0x7fff // in bytes, negative (15-bit) for bottom-up surfaces
#define MBX_2D_SURF_FMT_SHIFT  15
#define MBX_2D_SURF_FMT_MASK   0xf
#define MBX_2D_BLIT_ALPHA      (1 << 17)
#define MBX_2D_BLIT_CLIP       (1 << 18)
#define MBX_2D_BLIT_ROT_SHIFT  25 // 1-3: 90, 180, 270 degrees

// Pixel formats (format field of the surface headers)
#define MBX_FMT_ARGB4444       7
#define MBX_FMT_RGB555         8
#define MBX_FMT_ARGB1555       9
#define MBX_FMT_RGB565         10
#define MBX_FMT_RGB888         11
#define MBX_FMT_ARGB8888       12

// Time the cores take before they report completion
#define MBX_2D_DELAY_NS        (20 * SCALE_US)
#define MBX_3D_DELAY_NS        (100 * SCALE_US)

typedef struct MBX2DSurface {
    uint32_t addr;
    int32_t stride;
    uint32_t format;
} MBX2DSurface;

typedef struct IPodTouchMBXState {
    SysBusDevice busdev;
    MemoryRegion iomem1;
    MemoryRegion iomem2;
    qemu_irq irq;
    QEMUTimer *timer_2d;
    QEMUTimer *timer_3d;
    AddressSpace *dma_as;

    uint32_t regs[MBX_NUM_REGS];
    uint32_t wrapper_regs[MBX2_SIZE / 4];
    uint32_t int_status;
    uint32_t mmu_ctrl;

    // 2D block parser and the state the blocks set up
    uint32_t block[MBX_2D_MAX_BLOCK];
    uint32_t block_len;
    MBX2DSurface dst;
    MBX2DSurface src;
    uint32_t src_x;
    uint32_t src_y;
    uint32_t blend;
    uint32_t clip_x0, clip_x1, clip_y0, clip_y1;
    uint32_t scale_x;
    uint32_t scale_y;
} IPodTouchMBXState;

#endif
