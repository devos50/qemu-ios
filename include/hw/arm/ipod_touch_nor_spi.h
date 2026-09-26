#ifndef IPOD_TOUCH_NOR_SPI_H
#define IPOD_TOUCH_NOR_SPI_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/ssi/ssi.h"
#include "hw/hw.h"

#define TYPE_IPOD_TOUCH_NOR_SPI                "ipodtouch.norspi"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchNORSPIState, IPOD_TOUCH_NOR_SPI)

// AT25DF081A commands
#define NOR_WRITE_TO_STATUS_REG 0x1
#define NOR_WRITE_DATA_CMD      0x2
#define NOR_READ_DATA_CMD       0x3
#define NOR_DISABLE_WRITE       0x4
#define NOR_GET_STATUS_CMD      0x5
#define NOR_ENABLE_WRITE        0x6
#define NOR_FAST_READ_CMD       0xB
#define NOR_ERASE_BLOCK         0x20
#define NOR_ERASE_BLOCK_32K     0x52
#define NOR_ERASE_BLOCK_64K     0xD8
#define NOR_ERASE_CHIP          0x60
#define NOR_ERASE_CHIP_ALT      0xC7
#define NOR_GET_JEDECID         0x9F

#define NOR_STATUS_WEL          (1 << 1)
#define NOR_DEFAULT_SIZE        (1024 * 1024)

// The NOR contents, shared by the NOR devices on SPI0 (used by the bootrom and iBoot) and SPI1 (used by the kernel).
typedef struct IPodTouchNORImage {
    char *path;
    uint8_t *data;
    uint32_t size;
} IPodTouchNORImage;

typedef struct IPodTouchNORSPIState {
    SSIPeripheral ssidev;
    IPodTouchNORImage *image;

    // the current command and the number of bytes received since the chip was selected
    uint8_t cur_cmd;
    uint32_t cmd_pos;
    uint32_t addr;
    uint8_t status;

    // the range modified by the current command, written back to the image file on deselect
    uint32_t dirty_start;
    uint32_t dirty_end;
} IPodTouchNORSPIState;

IPodTouchNORImage *ipod_touch_nor_image_load(const char *path);

#endif
