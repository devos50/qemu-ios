#ifndef HW_ARM_IPOD_TOUCH_AES_H
#define HW_ARM_IPOD_TOUCH_AES_H

#include "qemu/osdep.h"
#include "hw/platform-bus.h"
#include "hw/hw.h"
#include "exec/hwaddr.h"
#include "exec/memory.h"
#include <openssl/aes.h>

#define TYPE_IPOD_TOUCH_AES                "ipodtouch.aes"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchAESState, IPOD_TOUCH_AES)

#define key_uid ((uint8_t[]){0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF})

#define AES_CONTROL 0x0
#define AES_GO 0x4
#define AES_UNKREG0 0x8
#define AES_STATUS 0xC
#define AES_UNKREG1 0x10
#define AES_MODE 0x14       // bit 0: encrypt, bits 4-5: key size (AESKeySize)
#define AES_SIZE 0x18
#define AES_CIPHER_ADDR 0x20 // the ciphertext side: input when decrypting, output when encrypting
#define AES_CIPHER_SIZE 0x24
#define AES_PLAIN_ADDR 0x28  // the plaintext side: output when decrypting, input when encrypting
#define AES_PLAIN_SIZE 0x2C
#define AES_AUXADDR 0x30
#define AES_AUXSIZE 0x34
#define AES_KEY_REG 0x4C     // a key of n bits takes the last n / 32 of these 8 words
#define AES_TYPE 0x6C
#define AES_IV_REG 0x74
#define AES_KEYSIZE 0x20
#define AES_IVSIZE 0x10

#define AES_MODE_ENCRYPT 0x1

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
    uint32_t ivec[4];
    uint32_t size;
    uint32_t cipher_addr;
    uint32_t plain_addr;
    uint32_t keytype;
    uint32_t status;
    uint32_t mode;
    uint32_t custkey[8];
} IPodTouchAESState;

#endif
