//  Copyright (c) 2013-2025 Jakub Filipowicz <jakubf@gmail.com>
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc.,
//  51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

#define _XOPEN_SOURCE 500
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <strings.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>
#include <uv.h>

#include "io/defs.h"
#include "io/cchar/cchar.h"
#include "io/cchar/uzdat.h"
#include "io/dev/terminal.h"

#include "log.h"

#define XFER_DIR_SWITCH_DELAY_MS 2

extern uv_loop_t *ioloop;

typedef struct uzdat_s uzdat_t;
struct uzdat_s {
	cchar_unit_t base;

	pthread_mutex_t mutex;
	int intspec;
	int state;
	int dir;
	bool xfer_busy;
	uint8_t buf_wr;

	// Hardware-faithful 1-byte receive buffer (the default): a character
	// arriving before the CPU read the previous one overwrites it and signals
	// CCHAR_INT_TOO_SLOW, exactly like the real UZ-DAT. The real CPU (~1 MIPS)
	// always drained it within one character time, so this never lost data in
	// practice.
	bool buf_rd_ready;
	uint8_t buf_rd;

	// "unattended" mode only (see terminal.h's `unattended` device option):
	// em400's CPU can stall much longer than the real one ever did (e.g. a
	// guest OS busy-looping elsewhere), long enough to lose typed input on an
	// automated/unattended session with nobody watching to retype a dropped
	// command. A small receive FIFO plus terminal-side backpressure
	// (uzdat_can_receive()) absorb such a stall instead. Left off by default -
	// deviates from the real 1-byte UZ-DAT, which had no such slack.
	bool unattended;
	uint8_t rx_fifo[32];
	unsigned rx_head;
	unsigned rx_count;

	em400_dev_t *dev;

	uv_async_t async_write;
	uv_async_t async_switch_transmit;
	uv_timer_t timer_switch_transmit;
	int open_handles;
};

enum uzdat_states {
	UZDAT_STATE_OFF,
	UZDAT_STATE_OK,
	UZDAT_STATE_EN
};

enum uzdat_dirs {
	UZDAT_DIR_NONE,
	UZDAT_DIR_IN,
	UZDAT_DIR_OUT
};

void uzdat_shutdown(cchar_unit_t *unit);
void uzdat_reset(cchar_unit_t *unit);
int uzdat_cmd(cchar_unit_t *unit, int dir, int cmd, uint16_t *r_arg);
int uzdat_intspec(cchar_unit_t *unit);
bool uzdat_has_interrupt(cchar_unit_t *unit);
bool uzdat_can_receive(void *ptr);

static void uzdat_on_async_write(uv_async_t *handle);
static void uzdat_on_async_switch_dir(uv_async_t *handle);

void uzdat_on_data_received(uzdat_t *uzdat, char data);

// -----------------------------------------------------------------------
// Receive FIFO helpers. Caller holds uzdat->mutex.
#define UZDAT_RX_FIFO_LEN ((unsigned) (sizeof(((uzdat_t*)0)->rx_fifo)))

static inline bool uzdat_rx_empty(uzdat_t *uzdat)
{
	return uzdat->rx_count == 0;
}

static inline bool uzdat_rx_full(uzdat_t *uzdat)
{
	return uzdat->rx_count >= UZDAT_RX_FIFO_LEN;
}

static inline void uzdat_rx_push(uzdat_t *uzdat, uint8_t data)
{
	unsigned tail = (uzdat->rx_head + uzdat->rx_count) % UZDAT_RX_FIFO_LEN;
	uzdat->rx_fifo[tail] = data;
	uzdat->rx_count++;
}

static inline uint8_t uzdat_rx_pop(uzdat_t *uzdat)
{
	uint8_t data = uzdat->rx_fifo[uzdat->rx_head];
	uzdat->rx_head = (uzdat->rx_head + 1) % UZDAT_RX_FIFO_LEN;
	uzdat->rx_count--;
	return data;
}

// Clears whichever receive buffer is in use. Caller holds uzdat->mutex.
static inline void uzdat_rx_flush(uzdat_t *uzdat)
{
	uzdat->rx_head = 0;
	uzdat->rx_count = 0;
	uzdat->buf_rd_ready = false;
}
void uzdat_on_data_sent(uzdat_t *uzdat);

// -----------------------------------------------------------------------
static void uzdat_try_free(uzdat_t *uzdat)
{
	if (uzdat->open_handles <= 0) {
		LOG(L_UZDAT, "No more open handles, UZDAT freeing resources");
		pthread_mutex_destroy(&uzdat->mutex);
		free(uzdat);
	}
}

// -----------------------------------------------------------------------
static void on_handle_close(uv_handle_t* handle)
{
	uzdat_t *uzdat = (uzdat_t *) uv_handle_get_data(handle);
	uzdat->open_handles--;
	uzdat_try_free(uzdat);
}

// -----------------------------------------------------------------------
static void uzdat_ioloop_teardown(uzdat_t *uzdat)
{
	if (!uv_is_closing((uv_handle_t *) &uzdat->async_write)) {
		uv_close((uv_handle_t *) &uzdat->async_write, on_handle_close);
	}
	if (!uv_is_closing((uv_handle_t *) &uzdat->async_switch_transmit)) {
		uv_close((uv_handle_t *) &uzdat->async_switch_transmit, on_handle_close);
	}
	if (!uv_is_closing((uv_handle_t *) &uzdat->timer_switch_transmit)) {
		uv_close((uv_handle_t *) &uzdat->timer_switch_transmit, on_handle_close);
	}
}

// -----------------------------------------------------------------------
static int uzdat_ioloop_setup(uzdat_t *uzdat)
{
	int res;

	res = uv_async_init(ioloop, &uzdat->async_write, uzdat_on_async_write);
	if (res) {
		return LOGERR("Device %i: UZDAT async_write handler init error: %s", uzdat->base.num, uv_strerror(res));
	}
	uv_handle_set_data((uv_handle_t*) &uzdat->async_write, uzdat);
	uzdat->open_handles++;

	res = uv_async_init(ioloop, &uzdat->async_switch_transmit, uzdat_on_async_switch_dir);
	if (res) {
		uv_close((uv_handle_t *) &uzdat->async_write, on_handle_close);
		return LOGERR("Device %i: UZDAT async_switch_transmit handler init error: %s", uzdat->base.num, uv_strerror(res));
	}
	uv_handle_set_data((uv_handle_t*) &uzdat->async_switch_transmit, uzdat);
	uzdat->open_handles++;

	res = uv_timer_init(ioloop, &uzdat->timer_switch_transmit);
	if (res) {
		uv_close((uv_handle_t *) &uzdat->async_write, on_handle_close);
		uv_close((uv_handle_t *) &uzdat->async_switch_transmit, on_handle_close);
		return LOGERR("Device %i: UZDAT timer init error: %s", uzdat->base.num, uv_strerror(res));
	}
	uv_handle_set_data((uv_handle_t*) &uzdat->timer_switch_transmit, uzdat);
	uzdat->open_handles++;

	return E_OK;
}

// -----------------------------------------------------------------------
cchar_unit_t * uzdat_create(int dev_num, em400_dev_t *dev)
{
	LOG(L_UZDAT, "Device %i: creating UZDAT terminal controller", dev_num);

	uzdat_t *uzdat = (uzdat_t*) calloc(1, sizeof(uzdat_t));
	if (!uzdat) {
		LOGERR("Device %i: failed to allocate memory for UZDAT", dev_num);
		return NULL;
	}

	if (pthread_mutex_init(&uzdat->mutex, NULL)) {
		LOGERR("Device %i: failed to initialize UZDAT mutex", dev_num);
		free(uzdat);
		return NULL;
	}

	uzdat->base.num = dev_num;
	uzdat->base.shutdown = uzdat_shutdown;
	uzdat->base.reset = uzdat_reset;
	uzdat->base.cmd = uzdat_cmd;
	uzdat->base.intspec = uzdat_intspec;
	uzdat->base.has_interrupt = uzdat_has_interrupt;

	uzdat_reset((cchar_unit_t *) uzdat);

	uzdat->dev = dev;
	uzdat->unattended = ((terminal_t *) dev)->unattended;
	// TODO: generic device callback registration?
	terminal_register_callbacks((terminal_t *) uzdat->dev, uzdat, (void*) uzdat_on_data_received, (void*) uzdat_on_data_sent, (void*) uzdat_can_receive);

	if (uzdat_ioloop_setup(uzdat) == E_ERR) {
		uzdat_try_free(uzdat);
		return NULL;
	}

	return (cchar_unit_t *) uzdat;
}

// -----------------------------------------------------------------------
void uzdat_on_data_received(uzdat_t *uzdat, char data)
{
	LOGCHAR(L_UZDAT, "%s: ", "Data received from terminal", data);

	int trigger_interrupt = false;

	pthread_mutex_lock(&uzdat->mutex);
	if ((uzdat->dir == UZDAT_DIR_IN) && (uzdat->state != UZDAT_STATE_OFF)) {
		if (uzdat->unattended) {
			if (uzdat_rx_full(uzdat)) {
				// genuine overrun: the CPU is not draining the FIFO at all
				uzdat->intspec = CCHAR_INT_TOO_SLOW;
			} else {
				uzdat_rx_push(uzdat, data);
				if (uzdat->state == UZDAT_STATE_EN) {
					uzdat->intspec = CCHAR_INT_READY;
				}
			}
		} else {
			// hardware-faithful: a single register, overwritten on overrun
			if (uzdat->buf_rd_ready) {
				uzdat->intspec = CCHAR_INT_TOO_SLOW;
			} else if (uzdat->state == UZDAT_STATE_EN) {
				uzdat->intspec = CCHAR_INT_READY;
			}
			uzdat->buf_rd = data;
			uzdat->buf_rd_ready = true;
		}
		trigger_interrupt = true;
	}
	pthread_mutex_unlock(&uzdat->mutex);

	if (trigger_interrupt) {
		cchar_int_trigger(uzdat->base.chan);
	}
}

// -----------------------------------------------------------------------
void uzdat_on_data_sent(uzdat_t *uzdat)
{
	LOG(L_UZDAT, "Data sent to terminal");

	int trigger_interrupt = false;

	pthread_mutex_lock(&uzdat->mutex);
	uzdat->xfer_busy = false;
	if (uzdat->state == UZDAT_STATE_EN) {
		uzdat->intspec = CCHAR_INT_READY;
		trigger_interrupt = true;
	}
	pthread_mutex_unlock(&uzdat->mutex);

	if (trigger_interrupt) {
		cchar_int_trigger(uzdat->base.chan);
	}
}

// -----------------------------------------------------------------------
static void uzdat_on_async_write(uv_async_t *handle)
{
	uzdat_t *uzdat = (uzdat_t*) handle->data;

	pthread_mutex_lock(&uzdat->mutex);
	uint8_t data = uzdat->buf_wr;
	pthread_mutex_unlock(&uzdat->mutex);

	if (!uzdat->dev) {
		LOGCHAR(L_UZDAT, "%s: ", "Failed write, no terminal connected", data);
		return;
	}

	int res = uzdat->dev->write(uzdat->dev, data);
	if (res == 0) {
		LOGCHAR(L_UZDAT, "%s: ", "Written to terminal", data);
	} else {
		LOGCHAR(L_UZDAT, "%s: ", "Failed write to terminal", data);
	}
}

// -----------------------------------------------------------------------
void uzdat_shutdown(cchar_unit_t *unit)
{
	if (!unit) return;

	LOG(L_UZDAT, "UZDAT shutting down");

	uzdat_t *uzdat = (uzdat_t*) unit;

	uzdat_ioloop_teardown(uzdat);
	if ((uzdat->dev) && (uzdat->dev->shutdown)) {
		uzdat->dev->shutdown((em400_dev_t *) uzdat->dev);
	}
}

// -----------------------------------------------------------------------
int uzdat_intspec(cchar_unit_t *unit)
{
	LOG(L_UZDAT, "Command: INTSPEC");
	uzdat_t *uzdat = (uzdat_t*) unit;

	pthread_mutex_lock(&uzdat->mutex);
	int spec = uzdat->intspec;
	uzdat->intspec = CCHAR_INT_OUTDATED;
	uzdat->state = UZDAT_STATE_OK;
	pthread_mutex_unlock(&uzdat->mutex);

	return spec;
}

// -----------------------------------------------------------------------
// Terminal-side backpressure, "unattended" mode only: true while there is
// room in the receive FIFO; false holds the next character in terminal.c
// instead of overrunning us. In the default (hardware-faithful) mode this
// always says yes, same as a controller with no opinion at all - terminal.c
// delivers on schedule and we do our own overwrite-on-overrun below.
bool uzdat_can_receive(void *ptr)
{
	uzdat_t *uzdat = (uzdat_t*) ptr;

	if (!uzdat->unattended) return true;

	pthread_mutex_lock(&uzdat->mutex);
	bool room = !uzdat_rx_full(uzdat);
	pthread_mutex_unlock(&uzdat->mutex);

	return room;
}

// -----------------------------------------------------------------------
bool uzdat_has_interrupt(cchar_unit_t *unit)
{
	uzdat_t *uzdat = (uzdat_t*) unit;

	pthread_mutex_lock(&uzdat->mutex);
	int intspec = uzdat->intspec;
	pthread_mutex_unlock(&uzdat->mutex);

	return intspec ? true : false;
}

// -----------------------------------------------------------------------
static int uzdat_read(uzdat_t *uzdat, uint16_t *r_arg)
{
	int ret;
	LOG(L_UZDAT, "Command: READ");

	uint8_t rx_data = 0;

	pthread_mutex_lock(&uzdat->mutex);
	uzdat->dir = UZDAT_DIR_IN;
	bool have_data = uzdat->unattended ? !uzdat_rx_empty(uzdat) : uzdat->buf_rd_ready;
	if (have_data) {
		rx_data = uzdat->unattended ? uzdat_rx_pop(uzdat) : uzdat->buf_rd;
		if (!uzdat->unattended) uzdat->buf_rd_ready = false;
		*r_arg = rx_data;
		uzdat->state = UZDAT_STATE_OK;
		ret = IO_OK;
	} else {
		uzdat->state = UZDAT_STATE_EN;
		ret = IO_EN;
	}
	pthread_mutex_unlock(&uzdat->mutex);

	if (ret == IO_OK) {
		LOGCHAR(L_UZDAT, "%s: ", "READ ready, received: ", rx_data);
	} else {
		LOG(L_UZDAT, "Buffer empty, nothing to read");
	}

	return ret;
}

// -----------------------------------------------------------------------
static void uzdat_on_transmit_switch_timeout(uv_timer_t *handle)
{
	uzdat_t *uzdat = (uzdat_t*) handle->data;

	bool trigger_interrupt = false;

	LOG(L_UZDAT, "Switched direction to transmit");

	pthread_mutex_lock(&uzdat->mutex);
	// only if UZDAT has not been reset
	if (uzdat->state != UZDAT_STATE_OFF) {
		uzdat->dir = UZDAT_DIR_OUT;
		if (uzdat->state == UZDAT_STATE_EN) {
			uzdat->intspec = CCHAR_INT_READY;
			trigger_interrupt = true;
		}
	}
	pthread_mutex_unlock(&uzdat->mutex);

	if (trigger_interrupt) {
		cchar_int_trigger(uzdat->base.chan);
	}
}

// -----------------------------------------------------------------------
static void uzdat_on_async_switch_dir(uv_async_t *handle)
{
	uzdat_t *uzdat = (uzdat_t*) handle->data;

	if (!uv_is_active((uv_handle_t*) &uzdat->timer_switch_transmit)) {
		uv_timer_start(&uzdat->timer_switch_transmit, uzdat_on_transmit_switch_timeout, XFER_DIR_SWITCH_DELAY_MS, 0);
	}
}

// -----------------------------------------------------------------------
static int uzdat_write(uzdat_t *uzdat, const uint16_t *r_arg)
{
	static const char *log_msgs[] = {
		"transceiver ready, sending",
		"transceiver not ready, not sending",
	};
	const char *log_msg = NULL;
	uv_async_t *trigger = NULL;
	int ret;

	LOG(L_UZDAT, "Command: WRITE");

	pthread_mutex_lock(&uzdat->mutex);
	if ((!uzdat->xfer_busy) && (uzdat->dir == UZDAT_DIR_OUT)) {
		uzdat->state = UZDAT_STATE_OK;
		uzdat->buf_wr = *r_arg;
		uzdat->xfer_busy = true;
		trigger = &uzdat->async_write;
		log_msg = log_msgs[0];
		ret = IO_OK;
	} else {
		if (uzdat->dir != UZDAT_DIR_OUT) {
			trigger = &uzdat->async_switch_transmit;
		}
		uzdat->state = UZDAT_STATE_EN;
		log_msg = log_msgs[1];
		ret = IO_EN;
	}
	pthread_mutex_unlock(&uzdat->mutex);

	LOGCHAR(L_UZDAT, "%s: ", log_msg, (uint8_t) *r_arg);
	if (trigger) {
		if (uv_async_send(trigger)) {
			LOG(L_UZDAT, "uzdat_write async trigger failed");
		}
	}
	return ret;
}

// -----------------------------------------------------------------------
static int uzdat_disconnect(uzdat_t *uzdat)
{
	LOG(L_UZDAT, "Command: DISCONNECT");

	int ret;

	pthread_mutex_lock(&uzdat->mutex);
	if (!uzdat->xfer_busy) {
		uzdat->state = UZDAT_STATE_OFF;
		uzdat->dir = UZDAT_DIR_NONE;
		uzdat_rx_flush(uzdat);
		ret = IO_OK;
	} else {
		uzdat->state = UZDAT_STATE_EN;
		ret = IO_EN;
	}
	pthread_mutex_unlock(&uzdat->mutex);

	return ret;
}

// -----------------------------------------------------------------------
void uzdat_reset(cchar_unit_t *unit)
{
	LOG(L_UZDAT, "UZDAT reset");

	uzdat_t *uzdat = (uzdat_t*) unit;

	pthread_mutex_lock(&uzdat->mutex);
	uzdat->state = UZDAT_STATE_OFF;
	uzdat->dir = UZDAT_DIR_NONE;
	uzdat->xfer_busy = false;
	uzdat_rx_flush(uzdat);
	pthread_mutex_unlock(&uzdat->mutex);
}

// -----------------------------------------------------------------------
int uzdat_cmd(cchar_unit_t *unit, int dir, int cmd, uint16_t *r_arg)
{
	uzdat_t *uzdat = (uzdat_t*) unit;

	if (dir == IO_IN) {
		switch (cmd) {
			case CCHAR_CMD_SPU:
				LOG(L_UZDAT, "Command: SPU");
				return IO_OK;
			case CCHAR_CMD_READ:
				return uzdat_read(uzdat, r_arg);
			default:
				LOG(L_UZDAT, "Unknown IN command: %i", cmd);
				return IO_NO;
		}
	} else {
		switch (cmd) {
			case CCHAR_CMD_RESET:
				LOG(L_UZDAT, "Command: RESET");
				uzdat_reset(unit);
				return IO_OK;
			case CCHAR_CMD_DETACH:
				return uzdat_disconnect(uzdat);
			case CCHAR_CMD_WRITE:
				return uzdat_write(uzdat, r_arg);
			default:
				LOG(L_UZDAT, "Unknown OU command: %i", cmd);
				return IO_NO;
		}
	}
}

// vim: tabstop=4 shiftwidth=4 autoindent
