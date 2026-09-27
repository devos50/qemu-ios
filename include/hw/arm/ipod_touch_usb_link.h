#ifndef HW_ARM_IPOD_TOUCH_USB_LINK_H
#define HW_ARM_IPOD_TOUCH_USB_LINK_H

#include "qemu/osdep.h"
#include "qemu/queue.h"
#include "chardev/char-fe.h"

/*
 * USB link between the emulated device controller and a host program over a
 * chardev. See ipod_touch_usb_link.c for the protocol.
 */

#define USB_LINK_VERSION 2

enum
{
	USB_LINK_HELLO = 0,
	USB_LINK_SETUP = 1,
	USB_LINK_OUT = 2,
	USB_LINK_IN = 3,
	USB_LINK_RESET = 4,
	USB_LINK_CANCEL = 5,
};

enum
{
	USB_LINK_OK = 0,
	USB_LINK_STALL = 1,
	USB_LINK_CANCELLED = 2,
	USB_LINK_NODEV = 3,
};

#define USB_LINK_MAX_EPS 16
#define USB_LINK_MAX_LENGTH (1 << 20)
#define USB_LINK_MAX_QUEUED (16 << 20)

typedef struct QEMU_PACKED usb_link_header
{
	uint8_t type;
	uint8_t ep;
	uint8_t status;
	uint8_t reserved;
	uint32_t tag;
	uint32_t length;
} usb_link_header;

typedef struct USBLinkRequest
{
	usb_link_header hdr;
	uint8_t *data;
	uint32_t actual;
	QTAILQ_ENTRY(USBLinkRequest) next;
} USBLinkRequest;

typedef QTAILQ_HEAD(, USBLinkRequest) USBLinkQueue;

typedef struct USBLinkOps
{
	// New SETUP, OUT or IN requests were queued.
	void (*kick)(void *opaque);
	// A bus reset reached the head of the reset queue. The controller
	// completes it with usb_link_complete once the guest has handled it.
	void (*reset)(void *opaque);
} USBLinkOps;

typedef struct USBLink
{
	CharBackend chr;
	uint32_t num_eps;
	const USBLinkOps *ops;
	void *opaque;

	// Endpoint queues, [0] for OUT and SETUP requests, [1] for IN requests.
	USBLinkQueue queues[2][USB_LINK_MAX_EPS];
	USBLinkQueue resets;
	uint64_t queued_bytes;

	usb_link_header rx_hdr;
	uint32_t rx_hdr_done;
	USBLinkRequest *rx_req;
	uint32_t rx_data_done;
} USBLink;

void usb_link_init(USBLink *link, uint32_t num_eps, const USBLinkOps *ops, void *opaque);

USBLinkRequest *usb_link_head(USBLink *link, bool is_in, uint8_t ep);
USBLinkRequest *usb_link_reset_head(USBLink *link);

// Answers a request and frees it.
void usb_link_complete(USBLink *link, USBLinkRequest *req, uint8_t status);

// Answers all queued endpoint requests with the given status.
void usb_link_abort_all(USBLink *link, uint8_t status);

// Drops all requests without answering them.
void usb_link_clear(USBLink *link);

#endif
