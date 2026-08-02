#ifndef DONGLE_STATUS_H
#define DONGLE_STATUS_H

/* Versioned payload returned by IAP command 0x91. Keep existing offsets stable;
 * future versions may append fields within the 64-byte HID response. */
#define DONGLE_STATUS_SCHEMA              1u
#define DONGLE_STATUS_PAYLOAD_LEN         32u

#define DONGLE_CONNECTION_UNAVAILABLE     0u
#define DONGLE_CONNECTION_PAIRING         1u
#define DONGLE_CONNECTION_WAITING         2u
#define DONGLE_CONNECTION_CONNECTED       3u

#define DONGLE_UPDATE_CLEAN               0u
#define DONGLE_UPDATE_STAGED              1u
#define DONGLE_UPDATE_PENDING             2u
#define DONGLE_UPDATE_UNAVAILABLE         0xffu

#define DONGLE_PROFILE_UNKNOWN            0u
#define DONGLE_PROFILE_PRODUCT            1u

#define DONGLE_CAP_RF                     0x01u
#define DONGLE_CAP_IAP                    0x02u
#define DONGLE_CAP_BOND                   0x04u
#define DONGLE_CAP_FAULT                  0x08u

#define DONGLE_UID_LEN                    8u

/* Payload offsets (response bytes begin with [ack][payload_len]). */
#define DONGLE_STATUS_OFF_SCHEMA          0u
#define DONGLE_STATUS_OFF_FAMILY          1u
#define DONGLE_STATUS_OFF_CONNECTION      2u
#define DONGLE_STATUS_OFF_UPDATE          3u
#define DONGLE_STATUS_OFF_CAPABILITIES    4u
#define DONGLE_STATUS_OFF_UID_LEN         5u
#define DONGLE_STATUS_OFF_PROFILE         6u
#define DONGLE_STATUS_OFF_RESERVED        7u
#define DONGLE_STATUS_OFF_UID             8u
#define DONGLE_STATUS_OFF_BUILD_ID       16u
#define DONGLE_STATUS_OFF_IMAGE_LEN      20u
#define DONGLE_STATUS_OFF_DONGLE_MAC     24u

#define DONGLE_MAC_LEN                    6u

#endif /* DONGLE_STATUS_H */
