#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "crypto/aes.h"
#include "system/physmem.h"
#include "hw/arm/ipod_touch_boot.h"
#include "hw/arm/ipod_touch_aes.h"

#define IMG2_MAGIC          0x494d4732  // 'IMG2'
#define IMG3_MAGIC          0x496d6733  // 'Img3'
#define IMG3_HEADER_SIZE    0x14        // magic, full size, size without padding, signed size, type
#define IMG3_TAG_HEADER     0xc         // magic, total size, data size

#define IMG3_TAG_DATA       0x44415441  // 'DATA'
#define IMG3_TAG_KBAG       0x4b424147  // 'KBAG'

#define KBAG_PLAIN          0           // the KBAG holds the IV and key as they are
#define KBAG_GID            1           // the IV and key are encrypted with the GID key

bool ipod_touch_boot_find_nor_image(const uint8_t *nor, size_t nor_size, uint32_t type,
                                    const uint8_t **image, size_t *image_size)
{
    /*
     * The IMG2 superblock: magic, image granule, image offset, boot block size, image space (in granules), ... The
     * images follow each other from (boot block size + image offset) granules on, like the bootrom reads them.
     */
    if (nor_size < 0x14 || ldl_le_p(nor) != IMG2_MAGIC) {
        return false;
    }
    uint64_t granule = ldl_le_p(nor + 0x4);
    uint64_t offset = (ldl_le_p(nor + 0xc) + (uint64_t)ldl_le_p(nor + 0x8)) * granule;

    while (offset + IMG3_HEADER_SIZE <= nor_size && ldl_le_p(nor + offset) == IMG3_MAGIC) {
        uint32_t full_size = ldl_le_p(nor + offset + 0x4);
        if (full_size < IMG3_HEADER_SIZE || offset + full_size > nor_size) {
            return false;
        }
        if (ldl_le_p(nor + offset + 0x10) == type) {
            *image = nor + offset;
            *image_size = full_size;
            return true;
        }
        offset += full_size;
    }
    return false;
}

/* Finds the first tag of a type in an IMG3 image; returns its data. */
static const uint8_t *img3_find_tag(const uint8_t *image, size_t size, uint32_t tag, uint32_t *data_size,
                                    const uint8_t *after)
{
    size_t offset = IMG3_HEADER_SIZE;
    size_t end = MIN(size, IMG3_HEADER_SIZE + (size_t)ldl_le_p(image + 0x8));

    while (offset + IMG3_TAG_HEADER <= end) {
        uint32_t total = ldl_le_p(image + offset + 0x4);
        uint32_t len = ldl_le_p(image + offset + 0x8);
        if (total < IMG3_TAG_HEADER || len > total - IMG3_TAG_HEADER || offset + total > end) {
            return NULL;
        }
        const uint8_t *data = image + offset + IMG3_TAG_HEADER;
        if (ldl_le_p(image + offset) == tag && (!after || data > after)) {
            *data_size = len;
            return data;
        }
        offset += total;
    }
    return NULL;
}

/* Gets the IV and key of an image from its KBAGs, like the bootrom: the first plain or GID-encrypted one. */
static bool img3_get_key(const uint8_t *image, size_t size, uint8_t *iv, uint8_t *key, uint32_t *key_bits,
                         Error **errp)
{
    const uint8_t *kbag = NULL;
    uint32_t len;
    bool found = false;

    while ((kbag = img3_find_tag(image, size, IMG3_TAG_KBAG, &len, kbag))) {
        if (len < 8) {
            continue;
        }
        uint32_t type = ldl_le_p(kbag);
        uint32_t bits = ldl_le_p(kbag + 4);
        uint32_t wrapped = AES_BLOCK_SIZE + bits / 8;
        uint8_t iv_key[AES_BLOCK_SIZE + 32];

        if ((bits != 128 && bits != 192 && bits != 256) || len < 8 + wrapped) {
            continue;
        }
        if (type == KBAG_PLAIN) {
            memcpy(iv_key, kbag + 8, wrapped);
        } else if (type == KBAG_GID) {
            found = true;
            if (!ipod_touch_aes_gid_decrypt(kbag + 8, iv_key, wrapped)) {
                continue;
            }
        } else {
            continue;
        }
        memcpy(iv, iv_key, AES_BLOCK_SIZE);
        memcpy(key, iv_key + AES_BLOCK_SIZE, bits / 8);
        *key_bits = bits;
        return true;
    }
    error_setg(errp, found ? "the key of this firmware image is not known (its KBAG is not in the AES key table)"
                           : "the image has no usable KBAG");
    return false;
}

bool ipod_touch_boot_load_img3(const uint8_t *image, size_t size, Error **errp)
{
    if (size < IMG3_HEADER_SIZE || ldl_le_p(image) != IMG3_MAGIC) {
        error_setg(errp, "not an IMG3 image");
        return false;
    }

    uint32_t len;
    const uint8_t *data = img3_find_tag(image, size, IMG3_TAG_DATA, &len, NULL);
    if (!data) {
        error_setg(errp, "the image has no DATA tag");
        return false;
    }
    if (len > IPOD_TOUCH_BOOT_MAX_SIZE) {
        error_setg(errp, "the image (%u bytes) is larger than the %u bytes the bootrom loads", len,
                   IPOD_TOUCH_BOOT_MAX_SIZE);
        return false;
    }

    g_autofree uint8_t *out = g_memdup2(data, len);
    uint32_t unused;
    if (img3_find_tag(image, size, IMG3_TAG_KBAG, &unused, NULL)) {
        uint8_t iv[AES_BLOCK_SIZE], key[32];
        uint32_t key_bits;
        AES_KEY aes;

        if (!img3_get_key(image, size, iv, key, &key_bits, errp)) {
            return false;
        }
        // CBC over the whole blocks; a trailing partial block is not encrypted
        AES_set_decrypt_key(key, key_bits, &aes);
        for (uint32_t off = 0; off + AES_BLOCK_SIZE <= len; off += AES_BLOCK_SIZE) {
            uint8_t block[AES_BLOCK_SIZE];
            AES_decrypt(data + off, block, &aes);
            for (int i = 0; i < AES_BLOCK_SIZE; i++) {
                out[off + i] = block[i] ^ iv[i];
            }
            memcpy(iv, data + off, AES_BLOCK_SIZE);
        }
    }

    physical_memory_write(IPOD_TOUCH_BOOT_ADDR, out, len);
    return true;
}
