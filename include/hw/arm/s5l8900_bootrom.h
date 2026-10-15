#ifndef HW_ARM_S5L8900_BOOTROM_H
#define HW_ARM_S5L8900_BOOTROM_H

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"

#define TYPE_S5L8900_BOOTROM "s5l8900.bootrom"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900BootromState, S5L8900_BOOTROM)

#define S5L8900_BOOTROM_ENTRIES     0x40        // the jump table at the start of the bootrom, 4 bytes an entry
#define S5L8900_BOOTROM_SIZE        0x10000     // iBoot copies all of it to check it against the bootroms it knows
#define S5L8900_BOOTROM_IO          0x1000      // the registers, over the ROM
#define S5L8900_BOOTROM_IO_SIZE     0x100

// the bootrom's entries iBoot 1.1.5 calls
#define S5L8900_ROM_USB_INIT        0x24        // with iBoot's USB context, after it has filled in the clock
#define S5L8900_ROM_8900_CHECK      0x2c        // (header, 1, 0): checks an 8900 container's header, 1 if good
#define S5L8900_ROM_8900_DECRYPT    0x30        // (header, data, ...): decrypts its data in place, 1 if good
#define S5L8900_ROM_USB_IRQ_27      0x34        // the handlers of interrupts 0x27, 0x28 and 0x13 (USB OTG)
#define S5L8900_ROM_USB_IRQ_28      0x38
#define S5L8900_ROM_USB_IRQ_OTG     0x3c
#define S5L8900_ROM_USB_START       0x44        // with iBoot's USB context

#define S5L8900_8900_MAGIC          0x30303938  // '8900'
#define S5L8900_8900_HEADER         0x800
#define S5L8900_8900_ENCRYPTED      3

typedef struct S5L8900BootromState {
    SysBusDevice parent_obj;

    MemoryRegion code;
    MemoryRegion io;

    uint32_t args[3];
    uint32_t result;
} S5L8900BootromState;

/*
 * Key 0x837: what the S5L8900's AES engine derives from its GID key, and what 8900 containers are encrypted with
 * (AES-128-CBC, zero IV).
 */
extern const uint8_t s5l8900_key_0x837[16];

/* AES-128-CBC over the whole blocks of buf, in place; a partial last block is left as it is, as the engine does. */
void s5l8900_aes_cbc(const uint8_t *key, const uint8_t *iv, uint8_t *buf, size_t len, bool encrypt);

#endif
