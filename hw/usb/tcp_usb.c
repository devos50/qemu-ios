/*
 * USB over TCP driver.
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
#include "hw/usb/tcp_usb.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#define DEBUG_TCP_USB

void tcp_usb_init(tcp_usb_state_t *_state, tcp_usb_callback_t _cb, tcp_usb_closed_t _closed, void *_arg)
{
	_state->data_callback = _cb;
	_state->closed_callback = _closed;
	_state->callback_arg = _arg;

	_state->socket = -1;
	_state->closed = 1;

	_state->state = tcp_usb_idle;
	_state->amount_done = 0;
}

void tcp_usb_cleanup(tcp_usb_state_t *_state)
{
	if(_state->socket >= 0)
	{
		close(_state->socket);
		qemu_set_fd_handler(_state->socket, NULL, NULL, NULL);
		_state->socket = -1;
	}
}

int tcp_usb_closed(tcp_usb_state_t *_state)
{
	return _state->closed != 0;
}

static void tcp_usb_do_closed(tcp_usb_state_t *_state)
{
	if(_state->closed)
		return;

	_state->closed = 1;

	qemu_set_fd_handler(_state->socket, NULL, NULL, NULL);

	if(_state->closed_callback)
		_state->closed_callback(_state, _state->callback_arg);
}

#ifdef DEBUG_TCP_USB
static void hexdump(const void *_data, size_t _amt)
{
	char *ptr = (char*)_data;
	int i;
	for(i = 0; i < _amt; i++)
	{
		if((i%8) == 0)
			fprintf(stderr, "\t");

		fprintf(stderr, "%02x ", (ptr[i] & 0xFF));

		if((i%8) == 7)
			fprintf(stderr, "\n");
	}

	if((i%8) != 0)
		fprintf(stderr, "\n");
}
#	define debug_hexdump(title, data, amt); fprintf(stderr, title); hexdump((data), (amt));
#	define debug_printf(args...) printf(args);
#else
#	define debug_hexdump(a, b, c)
#	define debug_printf(a...)
#endif // DEBUG_TCP_USB

static void tcp_usb_callback(tcp_usb_state_t *state, int _can_read, int _can_write)
{
	if(state->closed)
		return;

	int ret;
	switch(state->state)
	{
	case tcp_usb_idle:
		if(!_can_read)
			return;

		debug_printf("%s: tcp_usb_idle.\n", __func__);

		// Receiving new request
		state->header = malloc(sizeof(*state->header));
		state->amount_done = 0;
		state->state = tcp_usb_read_request;

		// Fall through
	case tcp_usb_read_request:
		debug_printf("%s: tcp_usb_read_request\n", __func__);

		if(state->amount_done < sizeof(*state->header))
		{
			ret = read(state->socket,
					((char*)state->header) + state->amount_done,
					sizeof(*state->header) - state->amount_done);
			if(ret == 0 && _can_read)
			{
				free(state->header);
				tcp_usb_do_closed(state);
				return;
			}
			else if(ret == EWOULDBLOCK || ret == 0 || ret == -1)
				return;
			else if(ret < 0)
			{
				fprintf(stderr, "tcp_usb: Error %d reading request header!\n", ret);
				return;
			}

			_can_read = 0;
			state->amount_done += ret;

			if(state->amount_done < sizeof(*state->header))
				return;

			debug_hexdump("tcp_usb: Got Header: ", state->header, sizeof(*state->header));
			debug_printf("Length: %d\n", state->header->length);

			if(state->header->length > 0)
				state->buffer = malloc(state->header->length);
			else
				state->buffer = NULL;
		}

		if((state->header->ep & USB_DIR_IN) == 0 && state->header->length > 0) // OUT
		{
			ret = read(state->socket,
					((char*)state->buffer) + (state->amount_done - sizeof(*state->header)),
					state->header->length - (state->amount_done - sizeof(*state->header)));
			printf("RET: %d\n", ret);
			if(ret == 0 && _can_read)
			{
				free(state->header);
				if(state->buffer)
					free(state->buffer);
				tcp_usb_do_closed(state);
				return;
			}
			else if(ret == EWOULDBLOCK || ret == 0 || ret == -1)
				return;
			else if(ret < 0)
			{
				printf("tcp_usb: Error %d reading request data!\n", ret);
				return;
			}

			_can_read = 0;
			state->amount_done += ret;

			if(state->amount_done < sizeof(*state->header) + state->header->length)
				return;

			debug_hexdump("tcp_usb: Read: ", state->buffer, state->header->length);
		}

		// Transfer complete! Call callback!
		if(state->data_callback)
		{
			state->state = tcp_usb_write_response;
			state->amount_done = 0;

			debug_printf("tcp_usb: Calling callback.\n");
			ret = state->data_callback(state, state->callback_arg, state->header, state->buffer);
			state->header->length = ret;
		}
		else
		{
			fprintf(stderr, "tcp_usb: Packet received but no callback!\n");
			state->state = tcp_usb_idle;
			return;
		}

		// Fall through
	case tcp_usb_write_response:
		debug_printf("%s: tcp_usb_write_response\n", __func__);

		if(state->amount_done < sizeof(*state->header))
		{
			ret = write(state->socket,
					((char*)state->header) + state->amount_done,
					sizeof(*state->header) - state->amount_done);
			if(ret == 0 && _can_write)
			{
				free(state->header);
				if(state->buffer)
					free(state->buffer);
				tcp_usb_do_closed(state);
				return;
			}
			else if(ret == EWOULDBLOCK || ret == 0 || ret == -1)
				return;
			else if(ret < 0)
			{
				fprintf(stderr, "tcp_usb: Error %d writing response header.\n", ret);
				return;
			}

			_can_write = 0;
			state->amount_done += ret;

			if(state->amount_done < sizeof(*state->header))
				return;
			
			debug_hexdump("tcp_usb: Sent Header: ", state->header, sizeof(*state->header));
		}

		if((state->header->ep & USB_DIR_IN) != 0 && state->header->length > 0) // IN
		{
			ret = write(state->socket,
					((char*)state->buffer) + (state->amount_done - sizeof(*state->header)),
					state->header->length - (state->amount_done - sizeof(*state->header)));
			if(ret == 0 && _can_write)
			{
				free(state->header);
				if(state->buffer)
					free(state->buffer);
				tcp_usb_do_closed(state);
				return;
			}
			else if(ret == EWOULDBLOCK || ret == 0 || ret == -1)
				return;
			else if(ret < 0)
			{
				fprintf(stderr, "tcp_usb: Error %d writing response data.\n", ret);
				return;
			}

			_can_write = 0;
			state->amount_done += ret;
		
			if(state->amount_done < sizeof(*state->header) + state->header->length)
				return;

			debug_hexdump("tcp_usb: Wrote: ", state->buffer, state->header->length);
		}

		free(state->header);
		if(state->buffer)
			free(state->buffer);
		state->state = tcp_usb_idle;
		break;

	case tcp_usb_write_request:
		debug_printf("%s: tcp_usb_write_request\n", __func__);

		if(state->amount_done < sizeof(*state->header))
		{
			ret = write(state->socket,
					((char*)state->header) + state->amount_done,
					sizeof(*state->header) - state->amount_done);
			if(ret == 0 && _can_write)
			{
				tcp_usb_do_closed(state);
				return;
			}
			else if(ret == EWOULDBLOCK || ret == 0 || ret == -1)
				return;
			else if(ret < 0)
			{
				fprintf(stderr, "tcp_usb: Error %d writing request header!\n", ret);
				return;
			}

			_can_write = 0;
			state->amount_done += ret;

			if(state->amount_done < sizeof(*state->header))
				return;

			debug_hexdump("tcp_usb: Sent Header: ", state->header, sizeof(*state->header));
		}

		if((state->header->ep & USB_DIR_IN) == 0 && state->header->length > 0) // OUT
		{
			ret = write(state->socket,
					((char*)state->buffer) + (state->amount_done - sizeof(*state->header)),
					state->header->length - (state->amount_done - sizeof(*state->header)));
			if(ret == 0 && _can_write)
			{
				tcp_usb_do_closed(state);
				return;
			}
			else if(ret == EWOULDBLOCK || ret == 0 || ret == -1)
				return;
			else if(ret < 0)
			{
				fprintf(stderr, "tcp_usb: Error %d writing request data!\n", ret);
				return;
			}

			_can_write = 0;
			state->amount_done += ret;

			if(state->amount_done < sizeof(*state->header) + state->header->length)
				return;
			
			debug_hexdump("tcp_usb: Wrote: ", state->buffer, state->header->length);
		}

		state->state = tcp_usb_read_response;
		state->amount_done = 0;

		// Fall through
	case tcp_usb_read_response:
		debug_printf("%s: tcp_usb_read_response\n", __func__);

		if(state->amount_done < sizeof(*state->header))
		{
			ret = read(state->socket,
					((char*)state->header) + state->amount_done,
					sizeof(*state->header) - state->amount_done);
			if(ret == 0 && _can_read)
			{
				tcp_usb_do_closed(state);
				return;
			}
			else if(ret == EWOULDBLOCK || ret == 0 || ret == -1)
				return;
			else if(ret < 0)
			{
				fprintf(stderr, "tcp_usb: Error %d reading response header.\n", ret);
				return;
			}

			_can_read = 0;
			state->amount_done += ret;

			if(state->amount_done < sizeof(*state->header))
				return;

			debug_hexdump("tcp_usb: Got Header: ", state->header, sizeof(*state->header));
		}
		
		if((state->header->ep & USB_DIR_IN) != 0 && state->header->length > 0) // IN
		{
			ret = read(state->socket,
					((char*)state->buffer) + (state->amount_done - sizeof(*state->header)),
					state->header->length - (state->amount_done - sizeof(*state->header)));
			if(ret == 0 && _can_read)
			{
				tcp_usb_do_closed(state);
				return;
			}
			else if(ret == EWOULDBLOCK || ret == 0 || ret == -1)
				return;
			else if(ret < 0)
			{
				fprintf(stderr, "tcp_usb: Error %d reading response data.\n", ret);
				return;
			}

			_can_read = 0;
			state->amount_done += ret;
		
			if(state->amount_done < sizeof(*state->header) + state->header->length)
				return;
			
			debug_hexdump("tcp_usb: Read: ", state->buffer, state->header->length);
		}

		state->state = tcp_usb_idle;

		// Transfer complete! Call callback!
		if(state->data_callback)
		{
			debug_printf("tcp_usb: calling callback!\n");
			state->data_callback(state, state->callback_arg, state->header, state->buffer);
			return;
		}
		else
		{
			fprintf(stderr, "tcp_usb: Request sent but no callback!\n");
			state->state = tcp_usb_idle;
			return;
		}
		break;
	}
}

static void tcp_usb_read_callback(void *_arg)
{
	tcp_usb_state_t *state = _arg;
	tcp_usb_callback(state, 1, 0);
}

static void tcp_usb_write_callback(void *_arg)
{
	tcp_usb_state_t *state = _arg;
	tcp_usb_callback(state, 0, 1);
}

int tcp_usb_connect(tcp_usb_state_t *_state, char *_host, uint32_t _port)
{
	_state->socket = socket(AF_INET, SOCK_STREAM, 0);
	
	struct hostent *hostname = gethostbyname(_host);

	struct sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(server_addr));

	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(_port);
	memcpy(&server_addr.sin_addr.s_addr,
			hostname->h_addr, hostname->h_length);

	int ret = connect(_state->socket, &server_addr, sizeof(server_addr));
	if(ret < 0)
		return -EIO;

	_state->closed = 0;
	int flags = fcntl(_state->socket, F_GETFL, 0);
	fcntl(_state->socket, F_SETFL, flags | O_NONBLOCK);
	qemu_set_fd_handler(_state->socket, tcp_usb_read_callback, tcp_usb_write_callback, _state);
	return 0;
}

int tcp_usb_request(tcp_usb_state_t *_state, tcp_usb_header_t *_header, const char *_data)
{
	debug_printf("%s.\n", __func__);

	if(_state->state != tcp_usb_idle)
		return -EBUSY;

	debug_printf("%s starting request.\n", __func__);
	
	_state->state = tcp_usb_write_request;
	_state->amount_done = 0;
	_state->buffer = (char*)_data;
	_state->header = _header;

	tcp_usb_callback(_state, 0, 1);
	return 0;
}

void tcp_usb_host_init(tcp_usb_host_state_t *_state)
{
	_state->socket = -1;
}

void tcp_usb_host_cleanup(tcp_usb_host_state_t *_state)
{
	if(_state->socket >= 0)
	{
		close(_state->socket);
		_state->socket = -1;
	}
}

int tcp_usb_host_okay(tcp_usb_host_state_t *_state)
{
	return 0;
}

int tcp_usb_host(tcp_usb_host_state_t *_state, uint32_t _port)
{
	_state->socket = socket(AF_INET, SOCK_STREAM, 0);
	if(_state->socket < 0)
		return -EIO;
	
	struct linger linger;
	linger.l_onoff = 0;
	linger.l_linger = 0;
	setsockopt(_state->socket, SOL_SOCKET, SO_LINGER, (char *)&linger, sizeof(linger));

	struct sockaddr_in server_addr;
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(_port);
	server_addr.sin_addr.s_addr = INADDR_ANY;

	if(bind(_state->socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0)
		return -EIO;

	listen(_state->socket, 5);
	return 0;
}

int tcp_usb_accept(tcp_usb_host_state_t *_host, tcp_usb_state_t *_client)
{
	struct sockaddr_in addr;
	size_t addr_sz = sizeof(addr);

	debug_printf("%s: waiting on accept...\n", __func__);

	_client->socket = accept(_host->socket, &addr, &addr_sz);
	if(_client->socket < 0)
	{
		fprintf(stderr, "%s: accept error %d.\n", __func__, errno);
		return -EIO;
	}

	_client->closed = 0;
	int flags = fcntl(_client->socket, F_GETFL, 0);
	fcntl(_client->socket, F_SETFL, flags | O_NONBLOCK);
	qemu_set_fd_handler(_client->socket, tcp_usb_read_callback, tcp_usb_write_callback, _client);
	debug_printf("%s: USB device accepted!\n", __func__);
	return 0;
}