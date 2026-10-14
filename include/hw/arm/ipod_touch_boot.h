#ifndef HW_ARM_IPOD_TOUCH_BOOT_H
#define HW_ARM_IPOD_TOUCH_BOOT_H

#include "qemu/osdep.h"
#include "exec/hwaddr.h"

/*
 * The first boot stage without the bootrom. The S5L8720 bootrom (iBoot-240.4) loads the DATA of an IMG3 image (LLB
 * from the NOR, or iBSS received over DFU) to the start of SRAM, decrypts it with the key in its KBAG (unwrapped with
 * the GID key) and jumps there with r0 = 0, the MMU and caches off and interrupts masked. Nothing after that calls
 * into the ROM or reads its data, so the machine can do this itself.
 */
#define IPOD_TOUCH_BOOT_ADDR      0x22000000
#define IPOD_TOUCH_BOOT_MAX_SIZE  0x24000     // the bootrom's own data starts at 0x22024000

#define IMG3_TAG_LLB              0x696c6c62  // 'illb'

/* Finds the image with the given type in the IMG2 image list at the start of a NOR. */
bool ipod_touch_boot_find_nor_image(const uint8_t *nor, size_t nor_size, uint32_t type,
                                    const uint8_t **image, size_t *image_size);

/* Decrypts the DATA of an IMG3 image and writes it to IPOD_TOUCH_BOOT_ADDR. */
bool ipod_touch_boot_load_img3(const uint8_t *image, size_t size, Error **errp);

#endif
