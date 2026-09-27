/*
 * USB link between an emulated device controller and a host program.
 *
 * The host sends requests over a chardev. Each request is a usb_link_header
 * (little endian), followed by a payload for SETUP (the 8-byte setup packet)
 * and OUT (the data of a whole transfer) requests:
 *
 * - HELLO: answered with the protocol version in the length field.
 * - SETUP / OUT / IN: a transfer on endpoint ep. For IN, length is the maximum
 *   number of bytes the host accepts.
 * - RESET: a bus reset.
 * - CANCEL: cancels the request whose tag is in the tag field.
 *
 * Every request is answered with a header that echoes type, ep and tag, and
 * carries a status and the number of bytes transferred. For IN, the data
 * follows the header. A CANCEL is answered with length 1 if it cancelled a
 * request and 0 if that request had already completed.
 *
 * Requests on the same endpoint and direction complete in order, but requests
 * on different endpoints are independent, so the host can keep an IN request
 * pending on one endpoint while it uses others. A request waits until the
 * guest arms its endpoint, so the host needs no delays between requests.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "hw/arm/ipod_touch_usb_link.h"
#include "trace.h"

USBLinkRequest *usb_link_head(USBLink *link, bool is_in, uint8_t ep)
{
	return QTAILQ_FIRST(&link->queues[is_in][ep]);
}

USBLinkRequest *usb_link_reset_head(USBLink *link)
{
	return QTAILQ_FIRST(&link->resets);
}

static USBLinkQueue *usb_link_queue_of(USBLink *link, USBLinkRequest *req)
{
	switch(req->hdr.type)
	{
	case USB_LINK_SETUP:
	case USB_LINK_OUT:
		return &link->queues[0][req->hdr.ep & 0x7f];
	case USB_LINK_IN:
		return &link->queues[1][req->hdr.ep & 0x7f];
	case USB_LINK_RESET:
		return &link->resets;
	default:
		return NULL;
	}
}

static void usb_link_free(USBLink *link, USBLinkRequest *req)
{
	link->queued_bytes -= req->hdr.length;
	g_free(req->data);
	g_free(req);
}

static void usb_link_reply(USBLink *link, usb_link_header *req_hdr, uint8_t status,
		uint32_t length, const uint8_t *data)
{
	usb_link_header hdr = *req_hdr;

	trace_ipod_touch_usb_link_reply(hdr.type, hdr.ep, hdr.tag, status, length);

	hdr.status = status;
	hdr.tag = cpu_to_le32(hdr.tag);
	hdr.length = cpu_to_le32(length);
	qemu_chr_fe_write_all(&link->chr, (uint8_t *)&hdr, sizeof(hdr));
	if(data && length)
		qemu_chr_fe_write_all(&link->chr, data, length);
}

void usb_link_complete(USBLink *link, USBLinkRequest *req, uint8_t status)
{
	bool is_reset = req->hdr.type == USB_LINK_RESET;
	bool with_data = req->hdr.type == USB_LINK_IN;

	QTAILQ_REMOVE(usb_link_queue_of(link, req), req, next);
	usb_link_reply(link, &req->hdr, status, req->actual, with_data ? req->data : NULL);
	usb_link_free(link, req);
	qemu_chr_fe_accept_input(&link->chr);

	if(is_reset && usb_link_reset_head(link))
		link->ops->reset(link->opaque);
}

void usb_link_abort_all(USBLink *link, uint8_t status)
{
	for(int dir = 0; dir < 2; dir++)
	{
		for(int ep = 0; ep < USB_LINK_MAX_EPS; ep++)
		{
			USBLinkRequest *req;
			while((req = QTAILQ_FIRST(&link->queues[dir][ep])))
				usb_link_complete(link, req, status);
		}
	}
}

void usb_link_clear(USBLink *link)
{
	for(int dir = 0; dir < 2; dir++)
	{
		for(int ep = 0; ep < USB_LINK_MAX_EPS; ep++)
		{
			USBLinkRequest *req;
			while((req = QTAILQ_FIRST(&link->queues[dir][ep])))
			{
				QTAILQ_REMOVE(&link->queues[dir][ep], req, next);
				usb_link_free(link, req);
			}
		}
	}

	USBLinkRequest *req;
	while((req = QTAILQ_FIRST(&link->resets)))
	{
		QTAILQ_REMOVE(&link->resets, req, next);
		usb_link_free(link, req);
	}

	if(link->rx_req)
		usb_link_free(link, link->rx_req);
	link->rx_req = NULL;
	link->rx_hdr_done = 0;
	link->rx_data_done = 0;
}

static void usb_link_cancel(USBLink *link, usb_link_header *hdr)
{
	uint32_t cancelled = 0;

	for(int dir = 0; dir < 2 && !cancelled; dir++)
	{
		for(int ep = 0; ep < USB_LINK_MAX_EPS && !cancelled; ep++)
		{
			USBLinkRequest *req;
			QTAILQ_FOREACH(req, &link->queues[dir][ep], next)
			{
				if(req->hdr.tag == hdr->tag)
				{
					usb_link_complete(link, req, USB_LINK_CANCELLED);
					cancelled = 1;
					break;
				}
			}
		}
	}

	usb_link_reply(link, hdr, USB_LINK_OK, cancelled, NULL);
}

static void usb_link_dispatch(USBLink *link, USBLinkRequest *req)
{
	trace_ipod_touch_usb_link_request(req->hdr.type, req->hdr.ep, req->hdr.tag, req->hdr.length);

	switch(req->hdr.type)
	{
	case USB_LINK_HELLO:
		usb_link_reply(link, &req->hdr, USB_LINK_OK, USB_LINK_VERSION, NULL);
		usb_link_free(link, req);
		return;

	case USB_LINK_CANCEL:
		usb_link_cancel(link, &req->hdr);
		usb_link_free(link, req);
		return;

	case USB_LINK_RESET:
		QTAILQ_INSERT_TAIL(&link->resets, req, next);
		if(usb_link_reset_head(link) == req)
			link->ops->reset(link->opaque);
		return;

	default:
		if((req->hdr.ep & 0x7f) >= link->num_eps)
		{
			usb_link_reply(link, &req->hdr, USB_LINK_STALL, 0, NULL);
			usb_link_free(link, req);
			return;
		}

		QTAILQ_INSERT_TAIL(usb_link_queue_of(link, req), req, next);
		link->ops->kick(link->opaque);
		return;
	}
}

static bool usb_link_has_payload(usb_link_header *hdr)
{
	return hdr->type == USB_LINK_SETUP || hdr->type == USB_LINK_OUT;
}

static bool usb_link_header_valid(usb_link_header *hdr)
{
	if((hdr->ep & 0x7f) >= USB_LINK_MAX_EPS || hdr->length > USB_LINK_MAX_LENGTH)
		return false;

	switch(hdr->type)
	{
	case USB_LINK_SETUP:
		return hdr->length == 8;
	case USB_LINK_HELLO:
	case USB_LINK_RESET:
	case USB_LINK_CANCEL:
		return hdr->length == 0;
	case USB_LINK_OUT:
	case USB_LINK_IN:
		return true;
	default:
		return false;
	}
}

static int usb_link_can_read(void *opaque)
{
	USBLink *link = opaque;

	if(link->queued_bytes >= USB_LINK_MAX_QUEUED)
		return 0;

	if(link->rx_hdr_done < sizeof(link->rx_hdr))
		return sizeof(link->rx_hdr) - link->rx_hdr_done;

	return link->rx_req->hdr.length - link->rx_data_done;
}

static void usb_link_read(void *opaque, const uint8_t *buf, int size)
{
	USBLink *link = opaque;

	while(size > 0)
	{
		if(link->rx_hdr_done < sizeof(link->rx_hdr))
		{
			uint32_t amt = MIN(size, sizeof(link->rx_hdr) - link->rx_hdr_done);
			memcpy((uint8_t *)&link->rx_hdr + link->rx_hdr_done, buf, amt);
			link->rx_hdr_done += amt;
			buf += amt;
			size -= amt;

			if(link->rx_hdr_done < sizeof(link->rx_hdr))
				return;

			link->rx_hdr.tag = le32_to_cpu(link->rx_hdr.tag);
			link->rx_hdr.length = le32_to_cpu(link->rx_hdr.length);
			if(!usb_link_header_valid(&link->rx_hdr))
			{
				error_report("usb_link: invalid request (type %u, ep 0x%02x, length %u), disconnecting",
						link->rx_hdr.type, link->rx_hdr.ep, link->rx_hdr.length);
				usb_link_clear(link);
				qemu_chr_fe_disconnect(&link->chr);
				return;
			}

			link->rx_req = g_new0(USBLinkRequest, 1);
			link->rx_req->hdr = link->rx_hdr;
			link->rx_req->data = g_malloc(MAX(link->rx_hdr.length, 1));
			link->rx_data_done = 0;
			link->queued_bytes += link->rx_hdr.length;
		}
		else
		{
			uint32_t amt = MIN(size, link->rx_req->hdr.length - link->rx_data_done);
			memcpy(link->rx_req->data + link->rx_data_done, buf, amt);
			link->rx_data_done += amt;
			buf += amt;
			size -= amt;
		}

		if(!usb_link_has_payload(&link->rx_req->hdr) || link->rx_data_done == link->rx_req->hdr.length)
		{
			USBLinkRequest *req = link->rx_req;
			link->rx_req = NULL;
			link->rx_hdr_done = 0;
			usb_link_dispatch(link, req);
		}
	}
}

static void usb_link_event(void *opaque, QEMUChrEvent event)
{
	USBLink *link = opaque;

	if(event == CHR_EVENT_CLOSED)
		usb_link_clear(link);
}

void usb_link_init(USBLink *link, uint32_t num_eps, const USBLinkOps *ops, void *opaque)
{
	link->num_eps = MIN(num_eps, USB_LINK_MAX_EPS);
	link->ops = ops;
	link->opaque = opaque;

	for(int dir = 0; dir < 2; dir++)
		for(int ep = 0; ep < USB_LINK_MAX_EPS; ep++)
			QTAILQ_INIT(&link->queues[dir][ep]);
	QTAILQ_INIT(&link->resets);

	qemu_chr_fe_set_handlers(&link->chr, usb_link_can_read, usb_link_read,
			usb_link_event, NULL, link, NULL, true);
}
