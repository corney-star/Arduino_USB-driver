#ifndef ARDUINO_USB_H
#define ARDUINO_USB_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define ARD_IOC_MAGIC		'A'

struct ard_usb_info {
	__u16 vendor_id;
	__u16 product_id;
	__u8 bulk_in_endpoint;
	__u8 bulk_out_endpoint;
	__u16 bulk_in_max_packet;
	__u8 interface_number;
	__u8 reserved;
};

#define ARD_IOCTL_GET_INFO	_IOR(ARD_IOC_MAGIC, 0x01, struct ard_usb_info)
#define ARD_IOCTL_CLEAR_ERRORS	_IO(ARD_IOC_MAGIC, 0x02)
#define ARD_IOCTL_CLEAR_HALT	_IO(ARD_IOC_MAGIC, 0x03)

#endif
