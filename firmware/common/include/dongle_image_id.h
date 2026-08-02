/* OpenDongle v2 application image identity and integrity header. */
#ifndef DONGLE_IMAGE_ID_H
#define DONGLE_IMAGE_ID_H

#include <stdint.h>

#define DONGLE_IMAGE_ID_OFF       0x20u
#define DONGLE_IMAGE_ID_LEN       0x20u
#define DONGLE_IMAGE_ID_CRC_OFF   0x10u

#define DONGLE_IMAGE_ID_MAGIC0 'O'
#define DONGLE_IMAGE_ID_MAGIC1 'D'
#define DONGLE_IMAGE_ID_MAGIC2 'G'
#define DONGLE_IMAGE_ID_MAGIC3 '2'
#define DONGLE_IMAGE_ID_FORMAT 2u

#define DONGLE_IMAGE_KIND_APP     0x01u
#define DONGLE_IMAGE_KIND_NON_APP 0x00u

/* image_crc32 is the zlib/IEEE CRC over image_len bytes with the four bytes
 * occupied by image_crc32 treated as zero.  That makes the finalized header
 * self-describing without a circular checksum dependency. */
typedef struct {
    uint8_t  magic[4];
    uint8_t  format_ver;
    uint8_t  family;
    uint8_t  image_kind;
    uint8_t  header_len;
    uint32_t base;
    uint32_t image_len;
    uint32_t image_crc32;
    uint32_t build_id;
    uint32_t flags;
    uint32_t extension_len;
} dongle_image_id_t;

_Static_assert(sizeof(dongle_image_id_t) == DONGLE_IMAGE_ID_LEN,
               "ODG2 header must be exactly 32 bytes");

/* Fixed fields checked before an updater accepts a staged image. */
#define DONGLE_IMAGE_ID_FIXED_LEN 12u

extern const dongle_image_id_t dongle_image_id;

#endif /* DONGLE_IMAGE_ID_H */
