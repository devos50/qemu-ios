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
 * USB link to the host.
 *
 * The host (e.g. a Python script) talks to this controller over a chardev.
 * It sends one request at a time as a usb_link_header, followed by the payload
 * for SETUP and OUT requests. SETUP carries the 8-byte setup packet, OUT the
 * data of a whole transfer, IN the maximum number of bytes the host accepts
 * and RESET signals a bus reset. Each request is answered with a header that
 * echoes type and ep, carries a status and, for IN, is followed by the data.
 *
 * A request waits until the guest arms the endpoint, and the controller stops
 * reading from the chardev until the request is answered. The host can
 * therefore send requests back to back without any delays.
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

static void synopsys_usb_link_reset(synopsys_usb_state *_state)
{
	g_free(_state->link_data);
	_state->link_data = NULL;
	_state->link_hdr_done = 0;
	_state->link_data_done = 0;
	_state->link_xfer_done = 0;
	_state->link_request_ready = false;
}

static void synopsys_usb_link_reply(synopsys_usb_state *_state, uint8_t _status, uint32_t _length, bool _with_data)
{
	usb_link_header hdr = _state->link_hdr;

	trace_ipod_touch_usb_link_reply(hdr.type, hdr.ep, _status, _length);

	hdr.status = _status;
	hdr.length = cpu_to_le32(_length);
	qemu_chr_fe_write_all(&_state->chr, (uint8_t *)&hdr, sizeof(hdr));
	if(_with_data && _length)
		qemu_chr_fe_write_all(&_state->chr, _state->link_data, _length);

	synopsys_usb_link_reset(_state);
	qemu_chr_fe_accept_input(&_state->chr);
}

static void synopsys_usb_ep_update_tsiz(synopsys_usb_ep_state *_eps, uint32_t _xfersize, uint32_t _pktcnt)
{
	_eps->tx_size = (_eps->tx_size & ~(DEPTSIZ_XFERSIZ_MASK | (DEPTSIZ_PKTCNT_MASK << DEPTSIZ_PKTCNT_SHIFT)))
		| (_xfersize & DEPTSIZ_XFERSIZ_MASK) | ((_pktcnt & DEPTSIZ_PKTCNT_MASK) << DEPTSIZ_PKTCNT_SHIFT);
}

// Moves packets between the pending request and an armed endpoint, like a
// host controller would. Returns true when the host side transfer is done.
static bool synopsys_usb_link_transfer(synopsys_usb_state *_state, synopsys_usb_ep_state *_eps,
		uint8_t _ep, bool _is_in)
{
	uint32_t length = _state->link_hdr.length;
	uint32_t mps = synopsys_usb_ep_mps(_eps, _ep);
	uint32_t xfersize = _eps->tx_size & DEPTSIZ_XFERSIZ_MASK;
	uint32_t pktcnt = (_eps->tx_size >> DEPTSIZ_PKTCNT_SHIFT) & DEPTSIZ_PKTCNT_MASK;
	bool pktcnt_limited = pktcnt != 0;
	bool host_done, guest_done;

	do
	{
		uint32_t pkt = MIN(MIN(mps, xfersize), length - _state->link_xfer_done);
		synopsys_usb_ep_dma(_state, _eps, _state->link_data + _state->link_xfer_done, pkt, !_is_in);

		_state->link_xfer_done += pkt;
		xfersize -= pkt;
		if(pktcnt_limited)
			pktcnt--;

		bool short_pkt = pkt < mps;
		guest_done = short_pkt || xfersize == 0 || (pktcnt_limited && pktcnt == 0);
		host_done = _state->link_xfer_done == length || (_is_in && short_pkt);
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

static void synopsys_usb_link_process(synopsys_usb_state *_state)
{
	if(!_state->link_request_ready)
		return;

	usb_link_header *hdr = &_state->link_hdr;
	uint8_t ep = hdr->ep & 0x7f;
	synopsys_usb_ep_state *eps;

	switch(hdr->type)
	{
	case USB_LINK_RESET:
		_state->gintsts |= GINTMSK_RESET | GINTMSK_ENUMDONE;
		synopsys_usb_update_irq(_state);
		synopsys_usb_link_reply(_state, USB_LINK_OK, 0, false);
		return;

	case USB_LINK_SETUP:
		eps = &_state->out_eps[ep];
		if(!(eps->control & USB_EPCON_ENABLE))
			return;

		// A SETUP packet clears a stalled control endpoint.
		eps->control &= ~(USB_EPCON_ENABLE | USB_EPCON_STALL);
		_state->in_eps[ep].control &= ~USB_EPCON_STALL;

		synopsys_usb_ep_dma(_state, eps, _state->link_data, 8, true);
		eps->interrupt_status |= USB_EPINT_SetUp;
		synopsys_usb_update_irq(_state);
		synopsys_usb_link_reply(_state, USB_LINK_OK, 8, false);
		return;

	case USB_LINK_OUT:
	case USB_LINK_IN:
	{
		bool is_in = hdr->type == USB_LINK_IN;
		eps = is_in ? &_state->in_eps[ep] : &_state->out_eps[ep];

		if(eps->control & USB_EPCON_STALL)
		{
			synopsys_usb_link_reply(_state, USB_LINK_STALL, 0, false);
			return;
		}

		if(!(eps->control & USB_EPCON_ENABLE))
			return;

		if(synopsys_usb_link_transfer(_state, eps, ep, is_in))
			synopsys_usb_link_reply(_state, USB_LINK_OK, _state->link_xfer_done, is_in);
		return;
	}
	}
}

static int synopsys_usb_link_can_read(void *_opaque)
{
	synopsys_usb_state *state = _opaque;

	if(state->link_request_ready)
		return 0;

	if(state->link_hdr_done < sizeof(state->link_hdr))
		return sizeof(state->link_hdr) - state->link_hdr_done;

	return state->link_hdr.length - state->link_data_done;
}

static bool synopsys_usb_link_header_valid(usb_link_header *_hdr)
{
	if((_hdr->ep & 0x7f) >= USB_NUM_ENDPOINTS || _hdr->length > USB_LINK_MAX_LENGTH)
		return false;

	switch(_hdr->type)
	{
	case USB_LINK_SETUP:
		return _hdr->length == 8;
	case USB_LINK_RESET:
		return _hdr->length == 0;
	case USB_LINK_OUT:
	case USB_LINK_IN:
		return true;
	default:
		return false;
	}
}

static void synopsys_usb_link_read(void *_opaque, const uint8_t *_buf, int _size)
{
	synopsys_usb_state *state = _opaque;

	while(_size > 0)
	{
		if(state->link_hdr_done < sizeof(state->link_hdr))
		{
			uint32_t amt = MIN(_size, sizeof(state->link_hdr) - state->link_hdr_done);
			memcpy((uint8_t *)&state->link_hdr + state->link_hdr_done, _buf, amt);
			state->link_hdr_done += amt;
			_buf += amt;
			_size -= amt;

			if(state->link_hdr_done < sizeof(state->link_hdr))
				return;

			state->link_hdr.length = le32_to_cpu(state->link_hdr.length);
			if(!synopsys_usb_link_header_valid(&state->link_hdr))
			{
				error_report("usb_synopsys: invalid USB link request (type %u, ep %u, length %u), disconnecting",
						state->link_hdr.type, state->link_hdr.ep, state->link_hdr.length);
				synopsys_usb_link_reset(state);
				qemu_chr_fe_disconnect(&state->chr);
				return;
			}

			state->link_data = g_malloc(MAX(state->link_hdr.length, 1));
		}
		else
		{
			uint32_t amt = MIN(_size, state->link_hdr.length - state->link_data_done);
			memcpy(state->link_data + state->link_data_done, _buf, amt);
			state->link_data_done += amt;
			_buf += amt;
			_size -= amt;
		}

		// IN and RESET requests carry no payload.
		bool has_payload = state->link_hdr.type == USB_LINK_SETUP || state->link_hdr.type == USB_LINK_OUT;
		if(!has_payload || state->link_data_done == state->link_hdr.length)
		{
			trace_ipod_touch_usb_link_request(state->link_hdr.type, state->link_hdr.ep, state->link_hdr.length);
			state->link_request_ready = true;
			synopsys_usb_link_process(state);
			return;
		}
	}
}

static void synopsys_usb_link_event(void *_opaque, QEMUChrEvent _event)
{
	synopsys_usb_state *state = _opaque;

	if(_event == CHR_EVENT_CLOSED)
		synopsys_usb_link_reset(state);
}

static void synopsys_usb_update_in_ep(synopsys_usb_state *_state, uint8_t _ep)
{
	synopsys_usb_ep_state *eps = &_state->in_eps[_ep];
	synopsys_usb_update_ep(_state, eps);

	if(eps->control & USB_EPCON_ENABLE)
		trace_ipod_touch_usb_ep_enable("in", _ep, eps->tx_size, eps->dma_address);

	synopsys_usb_link_process(_state);
}

static void synopsys_usb_update_out_ep(synopsys_usb_state *_state, uint8_t _ep)
{
	synopsys_usb_ep_state *eps = &_state->out_eps[_ep];
	synopsys_usb_update_ep(_state, eps);

	if(eps->control & USB_EPCON_ENABLE)
		trace_ipod_touch_usb_ep_enable("out", _ep, eps->tx_size, eps->dma_address);

	synopsys_usb_link_process(_state);
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

static uint64_t synopsys_usb_read(void *opaque, hwaddr _addr, unsigned size)
{
	synopsys_usb_state *state = (synopsys_usb_state *)opaque;
	
	//printf("USB: Read 0x%08x.\n", _addr);

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
	
	//printf("USB: Write 0x%08x to 0x%08x.\n", _val, _addr);

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

	synopsys_usb_link_reset(state);

	synopsys_usb_update_irq(state);
}

static void s5l8900_usb_otg_realize(DeviceState *dev, Error **errp)
{
	synopsys_usb_state *s = S5L8900USBOTG(dev);

	qemu_chr_fe_set_handlers(&s->chr, synopsys_usb_link_can_read, synopsys_usb_link_read,
			synopsys_usb_link_event, NULL, s, NULL, true);
}

static Property s5l8900_usb_otg_properties[] = {
	DEFINE_PROP_CHR("chardev", synopsys_usb_state, chr),
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
