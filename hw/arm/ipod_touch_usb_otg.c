/*
 * Synopsys DesignWareCore for USB OTG.
 *
 * Copyright (c) 2011 Richard Ian Taylor.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "qemu/osdep.h"
#include "hw/platform-bus.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/arm/ipod_touch_usb_otg.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "exec/cpu-common.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "trace.h"

static inline size_t synopsys_usb_tx_fifo_start(synopsys_usb_state *_state, uint32_t _fifo)
{
	if(_fifo == 0)
		return _state->gnptxfsiz >> 16;
	else
		return _state->dptxfsiz[_fifo-1] >> 16;
}

static inline size_t synopsys_usb_tx_fifo_size(synopsys_usb_state *_state, uint32_t _fifo)
{
	if(_fifo == 0)
		return _state->gnptxfsiz & 0xFFFF;
	else
		return _state->dptxfsiz[_fifo-1] & 0xFFFF;
}

static void synopsys_usb_update_irq(synopsys_usb_state *_state)
{
	_state->daintsts = 0;
	_state->gintsts &=~ (GINTMSK_OEP | GINTMSK_INEP | GINTMSK_OTG);

	if(_state->gotgint)
		_state->gintsts |= GINTMSK_OTG;

	int i;
	for(i = 0; i < USB_NUM_ENDPOINTS; i++)
	{
		if(_state->out_eps[i].interrupt_status & _state->doepmsk)
		{
			_state->daintsts |= 1 << (i+DAINT_OUT_SHIFT);
			if(_state->daintmsk & (1 << (i+DAINT_OUT_SHIFT)))
				_state->gintsts |= GINTMSK_OEP;
		}

		if(_state->in_eps[i].interrupt_status & _state->diepmsk)
		{
			_state->daintsts |= 1 << (i+DAINT_IN_SHIFT);
			if(_state->daintmsk & (1 << (i+DAINT_IN_SHIFT)))
				_state->gintsts |= GINTMSK_INEP;
		}
	}
	
	if((_state->pcgcctl & 3) == 0 && _state->gintmsk & _state->gintsts)
	{
		//printf("USB: IRQ triggered 0x%08x & 0x%08x.\n", _state->gintsts, _state->gintmsk);
		qemu_irq_raise(_state->irq);
	}
	else
		qemu_irq_lower(_state->irq);
}

static void synopsys_usb_update_ep(synopsys_usb_state *_state, synopsys_usb_ep_state *_ep)
{
	if(_ep->control & USB_EPCON_SETNAK)
	{
		_ep->control |= USB_EPCON_NAKSTS;
		_ep->interrupt_status |= USB_EPINT_INEPNakEff;
		_ep->control &=~ USB_EPCON_SETNAK;
	}

	if(_ep->control & USB_EPCON_DISABLE)
	{
		_ep->interrupt_status |= USB_EPINT_EPDisbld;
		_ep->control &=~ (USB_EPCON_DISABLE | USB_EPCON_ENABLE);
	}
}

/*
 * Device side of the USB link (see ipod_touch_usb_link.c). Requests queued by
 * the host are serviced once the guest arms the endpoint, and are split into
 * max-packet-size packets like a host controller would.
 */
static uint32_t synopsys_usb_ep_mps(synopsys_usb_ep_state *_eps, uint8_t _ep)
{
	if(_ep == 0)
		return 64 >> (_eps->control & 3);

	uint32_t mps = _eps->control & USB_EPCON_MPS_MASK;
	return mps ? mps : 64;
}

static void synopsys_usb_ep_dma(synopsys_usb_state *_state, synopsys_usb_ep_state *_eps,
		uint8_t *_buf, uint32_t _len, bool _is_write)
{
	if(!(_state->gahbcfg & GAHBCFG_DMAEN))
	{
		qemu_log_mask(LOG_UNIMP, "usb_synopsys: transfers without DMA are not supported\n");
		return;
	}

	cpu_physical_memory_rw(_eps->dma_address, _buf, _len, _is_write);
	_eps->dma_address += _len;
}

static void synopsys_usb_ep_update_tsiz(synopsys_usb_ep_state *_eps, uint32_t _xfersize, uint32_t _pktcnt)
{
	_eps->tx_size = (_eps->tx_size & ~(DEPTSIZ_XFERSIZ_MASK | (DEPTSIZ_PKTCNT_MASK << DEPTSIZ_PKTCNT_SHIFT)))
		| (_xfersize & DEPTSIZ_XFERSIZ_MASK) | ((_pktcnt & DEPTSIZ_PKTCNT_MASK) << DEPTSIZ_PKTCNT_SHIFT);
}

// Moves packets between a request and an armed endpoint. Returns true when
// the host side transfer is done.
static bool synopsys_usb_link_transfer(synopsys_usb_state *_state, synopsys_usb_ep_state *_eps,
		uint8_t _ep, bool _is_in, USBLinkRequest *_req)
{
	uint32_t length = _req->hdr.length;
	uint32_t mps = synopsys_usb_ep_mps(_eps, _ep);
	uint32_t xfersize = _eps->tx_size & DEPTSIZ_XFERSIZ_MASK;
	uint32_t pktcnt = (_eps->tx_size >> DEPTSIZ_PKTCNT_SHIFT) & DEPTSIZ_PKTCNT_MASK;
	bool pktcnt_limited = pktcnt != 0;
	bool host_done, guest_done;

	do
	{
		uint32_t pkt = MIN(MIN(mps, xfersize), length - _req->actual);
		synopsys_usb_ep_dma(_state, _eps, _req->data + _req->actual, pkt, !_is_in);

		_req->actual += pkt;
		xfersize -= pkt;
		if(pktcnt_limited)
			pktcnt--;

		bool short_pkt = pkt < mps;
		guest_done = short_pkt || xfersize == 0 || (pktcnt_limited && pktcnt == 0);
		host_done = _req->actual == length || (_is_in && short_pkt);
	} while(!guest_done && !host_done);

	synopsys_usb_ep_update_tsiz(_eps, xfersize, pktcnt);

	if(guest_done)
	{
		_eps->control &= ~USB_EPCON_ENABLE;
		_eps->interrupt_status |= USB_EPINT_XferCompl;
		synopsys_usb_update_irq(_state);
	}

	return host_done;
}

// Services the request at the head of an endpoint queue. Returns true if it
// completed, so the next one can be tried.
static bool synopsys_usb_service_ep(synopsys_usb_state *_state, bool _is_in, uint8_t _ep)
{
	USBLinkRequest *req = usb_link_head(&_state->link, _is_in, _ep);
	if(!req || _state->link_reset_phase != USB_LINK_RESET_IDLE)
		return false;

	synopsys_usb_ep_state *eps = _is_in ? &_state->in_eps[_ep] : &_state->out_eps[_ep];

	if(req->hdr.type == USB_LINK_SETUP)
	{
		if(!(eps->control & USB_EPCON_ENABLE))
			return false;

		// A SETUP packet clears a stalled control endpoint.
		eps->control &= ~(USB_EPCON_ENABLE | USB_EPCON_STALL);
		_state->in_eps[_ep].control &= ~USB_EPCON_STALL;

		synopsys_usb_ep_dma(_state, eps, req->data, 8, true);
		eps->interrupt_status |= USB_EPINT_SetUp;
		synopsys_usb_update_irq(_state);
		req->actual = 8;
		usb_link_complete(&_state->link, req, USB_LINK_OK);
		return true;
	}

	if(eps->control & USB_EPCON_STALL)
	{
		usb_link_complete(&_state->link, req, USB_LINK_STALL);
		return true;
	}

	if(!(eps->control & USB_EPCON_ENABLE))
		return false;

	if(!synopsys_usb_link_transfer(_state, eps, _ep, _is_in, req))
		return false;

	usb_link_complete(&_state->link, req, USB_LINK_OK);
	return true;
}

static void synopsys_usb_service(synopsys_usb_state *_state, bool _is_in, uint8_t _ep)
{
	while(synopsys_usb_service_ep(_state, _is_in, _ep))
		;
}

static void synopsys_usb_service_all(synopsys_usb_state *_state)
{
	for(int ep = 0; ep < USB_NUM_ENDPOINTS; ep++)
	{
		synopsys_usb_service(_state, false, ep);
		synopsys_usb_service(_state, true, ep);
	}
}

static void synopsys_usb_link_kick(void *_opaque)
{
	synopsys_usb_service_all(_opaque);
}

// A bus reset first raises USBRST and, once the guest acknowledges that,
// ENUMDONE. A real host then waits for the reset recovery time before it
// sends a SETUP. iBoot handles the reset in a task after acknowledging the
// interrupts, so the host request only completes once the guest re-arms EP0
// for the next SETUP, or after the recovery time if it never does.
static void synopsys_usb_link_bus_reset(void *_opaque)
{
	synopsys_usb_state *state = _opaque;

	usb_link_abort_all(&state->link, USB_LINK_CANCELLED);
	state->link_reset_phase = USB_LINK_RESET_SIGNALLED;
	state->gintsts |= GINTMSK_RESET;
	synopsys_usb_update_irq(state);
}

static void synopsys_usb_link_reset_done(synopsys_usb_state *_state, uint8_t _status)
{
	timer_del(_state->link_reset_timer);
	_state->link_reset_phase = USB_LINK_RESET_IDLE;

	USBLinkRequest *req = usb_link_reset_head(&_state->link);
	if(req)
		usb_link_complete(&_state->link, req, _status);

	synopsys_usb_service_all(_state);
}

static void synopsys_usb_link_reset_timeout(void *_opaque)
{
	synopsys_usb_state *state = _opaque;

	if(state->link_reset_phase == USB_LINK_RESET_RECOVERY)
		synopsys_usb_link_reset_done(state, USB_LINK_OK);
}

static void synopsys_usb_link_reset_step(synopsys_usb_state *_state)
{
	if(_state->link_reset_phase == USB_LINK_RESET_SIGNALLED && !(_state->gintsts & GINTMSK_RESET))
	{
		_state->link_reset_phase = USB_LINK_RESET_ENUMERATED;
		_state->gintsts |= GINTMSK_ENUMDONE;
		synopsys_usb_update_irq(_state);
	}
	else if(_state->link_reset_phase == USB_LINK_RESET_ENUMERATED && !(_state->gintsts & GINTMSK_ENUMDONE))
	{
		_state->link_reset_phase = USB_LINK_RESET_RECOVERY;
		timer_mod(_state->link_reset_timer,
				qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + USB_LINK_RESET_RECOVERY_MS);
	}
}

// The device went away from the host's point of view (core reset or soft
// disconnect), so fail everything the host is waiting for.
static void synopsys_usb_link_disconnect(synopsys_usb_state *_state)
{
	usb_link_abort_all(&_state->link, USB_LINK_NODEV);
	if(_state->link_reset_phase != USB_LINK_RESET_IDLE)
		synopsys_usb_link_reset_done(_state, USB_LINK_NODEV);
}

static const USBLinkOps synopsys_usb_link_ops = {
	.kick = synopsys_usb_link_kick,
	.reset = synopsys_usb_link_bus_reset,
};

static void synopsys_usb_update_in_ep(synopsys_usb_state *_state, uint8_t _ep)
{
	synopsys_usb_ep_state *eps = &_state->in_eps[_ep];
	synopsys_usb_update_ep(_state, eps);

	if(eps->control & USB_EPCON_ENABLE)
		trace_ipod_touch_usb_ep_enable("in", _ep, eps->tx_size, eps->dma_address);

	synopsys_usb_service(_state, true, _ep);
}

static void synopsys_usb_update_out_ep(synopsys_usb_state *_state, uint8_t _ep)
{
	synopsys_usb_ep_state *eps = &_state->out_eps[_ep];
	synopsys_usb_update_ep(_state, eps);

	if(eps->control & USB_EPCON_ENABLE)
	{
		trace_ipod_touch_usb_ep_enable("out", _ep, eps->tx_size, eps->dma_address);

		if(_ep == 0 && _state->link_reset_phase == USB_LINK_RESET_RECOVERY)
			synopsys_usb_link_reset_done(_state, USB_LINK_OK);
	}

	synopsys_usb_service(_state, false, _ep);
}

static uint32_t synopsys_usb_in_ep_read(synopsys_usb_state *_state, uint8_t _ep, hwaddr _addr)
{
	if(_ep >= USB_NUM_ENDPOINTS)
	{
		hw_error("usb_synopsys: Tried to read from disabled EP %d.\n", _ep);
		return 0;
	}

    switch (_addr)
	{
    case 0x00:
        return _state->in_eps[_ep].control;

    case 0x08:
        return _state->in_eps[_ep].interrupt_status;

    case 0x10:
        return _state->in_eps[_ep].tx_size;

    case 0x14:
        return _state->in_eps[_ep].dma_address;

    case 0x1C:
        return _state->in_eps[_ep].dma_buffer;

    default:
        hw_error("usb_synopsys: bad ep read offset 0x" HWADDR_FMT_plx "\n", _addr);
		break;
    }

	return 0;
}

static uint32_t synopsys_usb_out_ep_read(synopsys_usb_state *_state, int _ep, hwaddr _addr)
{
	if(_ep >= USB_NUM_ENDPOINTS)
	{
		hw_error("usb_synopsys: Tried to read from disabled EP %d.\n", _ep);
		return 0;
	}

    switch (_addr)
	{
    case 0x00:
        return _state->out_eps[_ep].control;

    case 0x08:
        return _state->out_eps[_ep].interrupt_status;

    case 0x10:
        return _state->out_eps[_ep].tx_size;

    case 0x14:
        return _state->out_eps[_ep].dma_address;

    case 0x1C:
        return _state->out_eps[_ep].dma_buffer;

    default:
        hw_error("usb_synopsys: bad ep read offset 0x" HWADDR_FMT_plx "\n", _addr);
		break;
    }

	return 0;
}

static uint64_t synopsys_usb_read_reg(synopsys_usb_state *state, hwaddr _addr);

static uint64_t synopsys_usb_read(void *opaque, hwaddr _addr, unsigned size)
{
	synopsys_usb_state *state = (synopsys_usb_state *)opaque;
	uint64_t val = synopsys_usb_read_reg(state, _addr);

	trace_ipod_touch_usb_read(_addr, val);
	return val;
}

static uint64_t synopsys_usb_read_reg(synopsys_usb_state *state, hwaddr _addr)
{
	switch(_addr)
	{
	case PCGCCTL:
		return state->pcgcctl;

	case GOTGCTL:
		return state->gotgctl;

	case GOTGINT:
		return state->gotgint;

	case GRSTCTL:
		return state->grstctl;

	case GHWCFG1:
		return state->ghwcfg1;

	case GHWCFG2:
		return state->ghwcfg2;

	case GHWCFG3:
		return state->ghwcfg3;

	case GHWCFG4:
		return state->ghwcfg4;

	case GAHBCFG:
		return state->gahbcfg;

	case GUSBCFG:
		return state->gusbcfg;

	case GINTMSK:
		return state->gintmsk;

	case GINTSTS:
		return state->gintsts;

	case DIEPMSK:
		return state->diepmsk;

	case DOEPMSK:
		return state->doepmsk;

	case DAINTMSK:
		return state->daintmsk;
	
	case DAINTSTS:
		return state->daintsts;

	case DCTL:
		return state->dctl;

	case DCFG:
		return state->dcfg;

	case DSTS:
		return state->dsts;

	case GRXSTSR:
	case GRXSTSP:
		return 0; // TODO: Do something about this?

	case GNPTXFSTS:
		return 0xFFFFFFFF;

	case HPRT0:
		// Host mode only. The kernel reads the line state (bits 10-11) in
		// device mode to detect a charger, which has D+ and D- both high.
		return 0;

	case GRXFSIZ:
		return state->grxfsiz;

	case GNPTXFSIZ:
		return state->gnptxfsiz;

	case DIEPTXF(1) ... DIEPTXF(USB_NUM_FIFOS+1):
		_addr -= DIEPTXF(1);
		_addr >>= 2;
		return state->dptxfsiz[_addr];

	case USB_INREGS ... (USB_INREGS + USB_EPREGS_SIZE - 4):
		_addr -= USB_INREGS;
		return synopsys_usb_in_ep_read(state, _addr >> 5, _addr & 0x1f);

	case USB_OUTREGS ... (USB_OUTREGS + USB_EPREGS_SIZE - 4):
		_addr -= USB_OUTREGS;
		return synopsys_usb_out_ep_read(state, _addr >> 5, _addr & 0x1f);

	case USB_FIFO_START ... USB_FIFO_END-4:
		_addr -= USB_FIFO_START;
		return *((uint32_t*)(&state->fifos[_addr]));

	default:
		hw_error("USB: Unhandled read address 0x%08x!\n", _addr);
	}

	return 0;
}

static void synopsys_usb_in_ep_write(synopsys_usb_state *_state, int _ep, hwaddr _addr, uint32_t _val)
{
	if(_ep >= USB_NUM_ENDPOINTS)
	{
		hw_error("usb_synopsys: Wrote to disabled EP %d.\n", _ep);
		return;
	}

    switch (_addr)
	{
    case 0x00:
		_state->in_eps[_ep].control = _val;
		synopsys_usb_update_in_ep(_state, _ep);
		return;

    case 0x08:
        _state->in_eps[_ep].interrupt_status &=~ _val;
		synopsys_usb_update_irq(_state);
		return;

    case 0x10:
        _state->in_eps[_ep].tx_size = _val;
		return;

    case 0x14:
        _state->in_eps[_ep].dma_address = _val;
		return;

    case 0x1C:
        _state->in_eps[_ep].dma_buffer = _val;
		return;

    default:
        hw_error("usb_synopsys: bad ep write offset 0x" HWADDR_FMT_plx "\n", _addr);
		break;
    }
}

static void synopsys_usb_out_ep_write(synopsys_usb_state *_state, int _ep, hwaddr _addr, uint32_t _val)
{
	if(_ep >= USB_NUM_ENDPOINTS)
	{
		hw_error("usb_synopsys: Wrote to disabled EP %d.\n", _ep);
		return;
	}

    switch (_addr)
	{
	case 0x00:
        _state->out_eps[_ep].control = _val;
		synopsys_usb_update_out_ep(_state, _ep);
		return;

    case 0x08:
        _state->out_eps[_ep].interrupt_status &=~ _val;
		synopsys_usb_update_irq(_state);
		return;

    case 0x10:
        _state->out_eps[_ep].tx_size = _val;
		return;

    case 0x14:
        _state->out_eps[_ep].dma_address = _val;
		return;

    case 0x1C:
        _state->out_eps[_ep].dma_buffer = _val;
		return;

    default:
        hw_error("usb_synopsys: bad ep write offset 0x" HWADDR_FMT_plx "\n", _addr);
		break;
    }
}

static void synopsys_usb_write(void *opaque, hwaddr _addr, uint64_t _val, unsigned size)
{
	synopsys_usb_state *state = (synopsys_usb_state *)opaque;

	trace_ipod_touch_usb_write(_addr, _val);

	switch(_addr)
	{
	case PCGCCTL:
		state->pcgcctl = _val;
		synopsys_usb_update_irq(state);
		return;

	case GOTGCTL:
		state->gotgctl = _val;
		break;

	case GOTGINT:
		state->gotgint &=~ _val;
		synopsys_usb_update_irq(state);
		return;

	case GRSTCTL:
		if(_val & GRSTCTL_CORESOFTRESET)
		{
			state->grstctl = GRSTCTL_AHBIDLE;
			synopsys_usb_link_disconnect(state);
			state->gintsts |= GINTMSK_RESET;
			synopsys_usb_update_irq(state);
		}
		else if(_val == 0)
			state->grstctl = _val;

		return;

	case GINTMSK:
		state->gintmsk = _val;
		synopsys_usb_update_irq(state);
		break;

	case GINTSTS:
		state->gintsts &=~ _val;
		synopsys_usb_update_irq(state);
		synopsys_usb_link_reset_step(state);
		return;

	case DOEPMSK:
		state->doepmsk = _val;
		synopsys_usb_update_irq(state);
		return;

	case DIEPMSK:
		state->diepmsk = _val;
		synopsys_usb_update_irq(state);
		return;

	case DAINTMSK:
		state->daintmsk = _val;
		synopsys_usb_update_irq(state);
		return;
	
	case DAINTSTS:
		state->daintsts &=~ _val;
		synopsys_usb_update_irq(state);
		return;

	case GAHBCFG:
		state->gahbcfg = _val;
		return;

	case GUSBCFG:
		state->gusbcfg = _val;
		return;

	case DCTL:
		if(_val & DCTL_CGNPINNAK)
			state->gintsts &= ~GINTMSK_GINNAKEFF;

		if(_val & DCTL_CGOUTNAK)
			state->gintsts &= ~GINTMSK_GOUTNAKEFF;

		_val &= ~(DCTL_CGNPINNAK | DCTL_CGOUTNAK);

		if((_val & DCTL_SGNPINNAK) != (state->dctl & DCTL_SGNPINNAK)
				&& (_val & DCTL_SGNPINNAK))
		{
			state->gintsts |= GINTMSK_GINNAKEFF;
			_val &=~ DCTL_SGNPINNAK;
		}

		if((_val & DCTL_SGOUTNAK) != (state->dctl & DCTL_SGOUTNAK)
				&& (_val & DCTL_SGOUTNAK))
		{
			state->gintsts |= GINTMSK_GOUTNAKEFF;
			_val &=~ DCTL_SGOUTNAK;
		}

		if((_val & DCTL_SFTDISCONNECT) && !(state->dctl & DCTL_SFTDISCONNECT))
			synopsys_usb_link_disconnect(state);

		state->dctl = _val;
		synopsys_usb_update_irq(state);
		return;

	case DCFG:
		//printf("USB: dcfg = 0x%08x.\n", _val);
		state->dcfg = _val;
		return;

	case GRXFSIZ:
		state->grxfsiz = _val;
		return;

	case GNPTXFSIZ:
		state->gnptxfsiz = _val;
		return;

	case DIEPTXF(1) ... DIEPTXF(USB_NUM_FIFOS+1):
		_addr -= DIEPTXF(1);
		_addr >>= 2;
		state->dptxfsiz[_addr] = _val;
		return;

	case USB_INREGS ... (USB_INREGS + USB_EPREGS_SIZE - 4):
		_addr -= USB_INREGS;
		synopsys_usb_in_ep_write(state, _addr >> 5, _addr & 0x1f, _val);
		return;

	case USB_OUTREGS ... (USB_OUTREGS + USB_EPREGS_SIZE - 4):
		_addr -= USB_OUTREGS;
		synopsys_usb_out_ep_write(state, _addr >> 5, _addr & 0x1f, _val);
		return;

	case USB_FIFO_START ... USB_FIFO_END-4:
		_addr -= USB_FIFO_START;
		*((uint32_t*)(&state->fifos[_addr])) = _val;
		return;

	default:
		hw_error("USB: Unhandled write address 0x%08x!\n", _addr);
	}
}

static const MemoryRegionOps usb_otg_ops = {
    .read = synopsys_usb_read,
    .write = synopsys_usb_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void s5l8900_usb_otg_reset(DeviceState *d)
{
	synopsys_usb_state *state = S5L8900USBOTG(d);

	state->pcgcctl = 3;
	state->grstctl = GRSTCTL_AHBIDLE;

	state->gahbcfg = 0;
	state->gusbcfg = 0;

	state->dctl = 0;
	state->dcfg = 0;
	state->dsts = 0;

	state->gotgctl = 0;
	state->gotgint = 0;

	state->gintmsk = 0;
	state->gintsts = 0;

	state->daintmsk = 0;
	state->daintsts = 0;

	state->diepmsk = 0;
	state->doepmsk = 0;

	state->grxfsiz = 0x100;
	state->gnptxfsiz = (0x100 << 16) | 0x100;

	uint32_t counter = 0x200;
	int i;
	for(i = 0; i < USB_NUM_FIFOS; i++)
	{
		state->dptxfsiz[i] = (counter << 16) | 0x100;
		counter += 0x100;
	}

	for(i = 0; i < USB_NUM_ENDPOINTS; i++)
	{
		synopsys_usb_ep_state *in = &state->in_eps[i];
		in->control = 0;
		in->dma_address = 0;
		in->fifo = 0;
		in->tx_size = 0;

		synopsys_usb_ep_state *out = &state->out_eps[i];
		out->control = 0;
		out->dma_address = 0;
		out->fifo = 0;
		out->tx_size = 0;
	}

	synopsys_usb_link_disconnect(state);

	synopsys_usb_update_irq(state);
}

static void s5l8900_usb_otg_realize(DeviceState *dev, Error **errp)
{
	synopsys_usb_state *s = S5L8900USBOTG(dev);

	s->link_reset_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, synopsys_usb_link_reset_timeout, s);
	usb_link_init(&s->link, USB_NUM_ENDPOINTS, &synopsys_usb_link_ops, s);
}

static Property s5l8900_usb_otg_properties[] = {
	DEFINE_PROP_CHR("chardev", synopsys_usb_state, link.chr),
	DEFINE_PROP_END_OF_LIST(),
};

static void s5l8900_usb_otg_init1(Object *obj)
{
    synopsys_usb_state *s = S5L8900USBOTG(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, OBJECT(s), &usb_otg_ops, s, "usb_otg", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

// Helper for adding to a machine
DeviceState *ipod_touch_init_usb_otg(qemu_irq _irq, uint32_t _hwcfg[4])
{
	DeviceState *dev = qdev_new(TYPE_S5L8900USBOTG);
	synopsys_usb_state *state = S5L8900USBOTG(dev);

	state->ghwcfg1 = _hwcfg[0];
	state->ghwcfg2 = _hwcfg[1];
	state->ghwcfg3 = _hwcfg[2];
	state->ghwcfg4 = _hwcfg[3];

	SysBusDevice *sdev = SYS_BUS_DEVICE(dev);
    sysbus_connect_irq(sdev, 0, _irq);

    return dev;
}

static void s5l8900_usb_otg_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->reset = s5l8900_usb_otg_reset;
    dc->realize = s5l8900_usb_otg_realize;
    device_class_set_props(dc, s5l8900_usb_otg_properties);
}

static const TypeInfo s5l8900_usb_otg_info = {
    .name          = TYPE_S5L8900USBOTG,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(synopsys_usb_state),
    .instance_init = s5l8900_usb_otg_init1,
    .class_init    = s5l8900_usb_otg_class_init,
};

static void s5l8900_usb_otg_register_types(void)
{
    type_register_static(&s5l8900_usb_otg_info);
}

type_init(s5l8900_usb_otg_register_types)
