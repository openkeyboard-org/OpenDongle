#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H

#include <stdint.h>

extern const uint8_t usb_device_desc[18];
extern const uint8_t usb_config_desc[141];
extern const uint8_t usb_lang_desc[4];
extern const uint8_t usb_product_desc[32];

extern const uint8_t usb_hid_report_desc_if0[72];
extern const uint8_t usb_hid_report_desc_if1[61];
extern const uint8_t usb_hid_report_desc_if2[205];
extern const uint8_t usb_hid_report_desc_if3[21];
extern const uint8_t usb_hid_report_desc_if4[34];

#define HID_REPORT_DESC_SIZE_IF0  72
#define HID_REPORT_DESC_SIZE_IF1  61
#define HID_REPORT_DESC_SIZE_IF2  205
#define HID_REPORT_DESC_SIZE_IF3  21
#define HID_REPORT_DESC_SIZE_IF4  34

#define USB_NUM_INTERFACES  5

#endif /* USB_DESCRIPTORS_H */
