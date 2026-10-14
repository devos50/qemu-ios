#ifndef HW_ARM_IPOD_TOUCH_AES_H
#define HW_ARM_IPOD_TOUCH_AES_H

#include "qemu/osdep.h"
#include "hw/core/platform-bus.h"
#include "hw/core/hw-error.h"
#include "hw/core/irq.h"
#include "exec/hwaddr.h"
#include "system/memory.h"
#include "crypto/aes.h"

#define TYPE_IPOD_TOUCH_AES                "ipodtouch.aes"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchAESState, IPOD_TOUCH_AES)

#define key_uid ((uint8_t[]){0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF})

/*
 * An operation processes AES_SIZE bytes from the input segment (AES_IN_ADDR/AES_IN_SIZE) to the output segment
 * (AES_OUT_ADDR/AES_OUT_SIZE), in both directions. A buffer that is not physically contiguous is given one segment at
 * a time: when the engine has consumed the input segment or filled the output segment, it stops and reports this in
 * AES_STATUS, the driver programs the next segment and continues the operation with AES_GO_CONTINUE.
 */
#define AES_CONTROL 0x0
#define AES_GO 0x4
#define AES_RESET 0x8        // writing 1 clears the status
#define AES_STATUS 0xC       // AES_STATUS_*, write 1 to clear
#define AES_IRQ_ENABLE 0x10  // the AES_STATUS bits that raise the interrupt
#define AES_MODE 0x14        // bit 0: encrypt, bits 4-5: key size (AESKeySize)
#define AES_SIZE 0x18
#define AES_OUT_ADDR 0x20
#define AES_OUT_SIZE 0x24
#define AES_IN_ADDR 0x28
#define AES_IN_SIZE 0x2C
#define AES_AUXADDR 0x30     // a copy of the input segment (iBoot) or 0 (the kernel); unused
#define AES_AUXSIZE 0x34
#define AES_KEY_REG 0x4C     // a key of n bits takes the last n / 32 of these 8 words
#define AES_TYPE 0x6C
#define AES_IV_REG 0x74
#define AES_KEYSIZE 0x20
#define AES_IVSIZE 0x10

#define AES_MODE_ENCRYPT 0x1

#define AES_GO_START 1
#define AES_GO_CONTINUE 3

#define AES_STATUS_DONE 0x1
#define AES_STATUS_OUT_FULL 0x2   // the output segment is full
#define AES_STATUS_IN_EMPTY 0x4   // the input segment is consumed
#define AES_STATUS_MASK 0x7

typedef enum AESKeyType {
    AESCustom = 0,
    AESGID = 1,
    AESUID = 2
} AESKeyType;

typedef enum AESKeySize {
    AES128 = 0,
    AES192 = 1,
    AES256 = 2
} AESKeySize;

typedef struct IPodTouchAESState
{
    SysBusDevice busdev;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t ivec[4];
    uint32_t size;
    uint32_t out_addr;
    uint32_t out_size;
    uint32_t in_addr;
    uint32_t in_size;
    uint32_t keytype;
    uint32_t status;
    uint32_t irq_enable;
    uint32_t mode;
    uint32_t custkey[8];

    // the operation in progress, which waits for the next segment while remaining != 0
    uint32_t remaining;
    AES_KEY key;
} IPodTouchAESState;

/*
 * Answers a GID decryption of a KBAG from the table of known image keys: writes the IV and key to out and returns
 * true if the KBAG is known.
 */
bool ipod_touch_aes_gid_decrypt(const uint8_t *in, uint8_t *out, uint32_t size);

#endif
