//  Copyright (c) 2026 Marcin Golesz
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

// PLIX - Amepol's intelligent peripheral processor for MERA 9425/EC 5061 disks,
// PT 305 tape and (later) Winchester drives. Reference: "Styk MERA-PLIX,
// wersja F", Amepol, luty 1987.
//
// This is a Tier 0 STUB: enough of the channel-command / general-control /
// line-control protocol to let CROOK-5's INI register the channel and any
// ASG/LOAP/INPR on a PLIX device resolve instead of failing with Err 23 - no
// package/line state is modeled and no actual device can be attached yet
// (max_devices == 0). A real disk/tape controller (Tier 1) would replace the
// line-control handlers below with an actual transmit state machine driving
// an em400_dev_t, most likely following the shape of src/io/mx/mx.c.

#include <stdlib.h>
#include <inttypes.h>

#include "libem400.h"
#include "log.h"
#include "io/io.h"
#include "io/defs.h"
#include "io/chan.h"
#include "io/plix/plix.h"
#include "utils/elst.h"

// Command word (all IN/OU): Q=0, N15=0, N11-14 = channel number (already
// consumed by io_dispatch() before we get here). Of the remaining bits:
//   op    = N0-2  (bits 15-13) - command group
//   subop = N3-4  (bits 12-11) - channel-command sub-op
//   line  = N3-10 (bits 12-5)  - logical line id, for line-control commands
enum plix_op {
	PLIX_OP_CHAN    = 0, // channel commands (IN only)
	PLIX_OP_GENERAL = 1, // cofnij przerwanie (IN) / testuj (OU)
	PLIX_OP_ATTACH  = 2, // dołącz linię (OU) / odłącz linię (IN)
	PLIX_OP_STATUS  = 3, // podaj status (OU) / zerwij transmisję (IN)
	PLIX_OP_XMIT    = 4, // przesyłaj (OU) / zeruj urządzenie (IN)
	PLIX_OP_SETCFG  = 5, // ustaw konfigurację (OU)
};

enum plix_chan_subop {
	PLIX_CHAN_RESET  = 0, // zeruj moduł
	PLIX_CHAN_ISPEC  = 1, // podaj specyfikację (== CHAN_CMD_INTSPEC<<10)
	PLIX_CHAN_EXISTS = 2, // sprawdź istnienie
};

// Interrupt numbers, "Styk MERA-PLIX" table.
enum plix_interrupt {
	PLIX_INSKA = 1,  // niesprawny kanał
	PLIX_IWYZE = 2,  // wykonano zerowanie
	PLIX_IWYTE = 3,  // wykonano test
	PLIX_INKON = 4,  // ustaw konfigurację: odrzucona
	PLIX_IUKON = 5,  // ustaw konfigurację: przyjęta
	PLIX_ISTRE = 7,  // podaj status: powodzenie
	PLIX_IDOLI = 10, // dołącz linię: powodzenie
	PLIX_IETRA = 13, // przesyłaj: powodzenie
	PLIX_IABTR = 20, // zerwij: powodzenie
	PLIX_IODLI = 23, // odłącz linię: powodzenie
	PLIX_IZURZ = 35, // zeruj urządzenie: powodzenie
};

struct plix_int {
	uint8_t number;
	uint8_t line;
};

typedef struct chan_plix {
	chan_t base;
	ELST intq; // pending struct plix_int*, oldest first
} chan_plix_t;

void plix_shutdown(chan_t *chan);
void plix_reset(chan_t *chan);
int plix_cmd(chan_t *chan, int dir, uint16_t n_arg, uint16_t *r_arg);

// -----------------------------------------------------------------------
bool plix_dev_compatible(int dev_type)
{
	// Tier 0: no device can be attached to a PLIX channel yet.
	(void) dev_type;
	return false;
}

// -----------------------------------------------------------------------
static void plix_int_destructor(void *ptr)
{
	free(ptr);
}

// -----------------------------------------------------------------------
static void plix_int_push(chan_plix_t *plix, uint8_t number, uint8_t line)
{
	struct plix_int *i = malloc(sizeof(struct plix_int));
	if (!i) return;
	i->number = number;
	i->line = line;
	elst_append(plix->intq, i);

	LOG(L_PLIX, "PLIX %i: queued interrupt %i, line %i", plix->base.num, number, line);
	io_int_set(plix->base.num);
}

// -----------------------------------------------------------------------
static int plix_cmd_chan(chan_plix_t *plix, int subop, uint16_t *r_arg)
{
	switch (subop) {
		case PLIX_CHAN_RESET:
			LOG(L_PLIX, "PLIX %i: reset module", plix->base.num);
			elst_clear(plix->intq);
			plix_int_push(plix, PLIX_IWYZE, 0);
			*r_arg = 0;
			return IO_OK;
		case PLIX_CHAN_ISPEC: {
			struct plix_int *i = elst_pop(plix->intq);
			if (i) {
				*r_arg = ((uint16_t) i->number) | ((uint16_t) i->line << 8);
				LOG(L_PLIX, "PLIX %i: intspec -> int %i, line %i", plix->base.num, i->number, i->line);
				free(i);
			} else {
				*r_arg = 0;
			}
			return IO_OK;
		}
		case PLIX_CHAN_EXISTS:
			LOG(L_PLIX, "PLIX %i: module exists", plix->base.num);
			*r_arg = 0;
			return IO_OK;
		default:
			*r_arg = 0;
			return IO_OK;
	}
}

// -----------------------------------------------------------------------
int plix_cmd(chan_t *chan, int dir, uint16_t n_arg, uint16_t *r_arg)
{
	chan_plix_t *plix = (chan_plix_t *) chan;

	int op = (n_arg >> 13) & 0b111;
	int subop = (n_arg >> 11) & 0b11;
	int line = (n_arg >> 5) & 0xff;

	if (dir == IO_IN) *r_arg = 0;

	LOG(L_PLIX, "PLIX %i: %s op=%i subop/line=%i, n_arg=0x%04x", chan->num,
		dir == IO_IN ? "IN" : "OU", op, dir == IO_IN && op == PLIX_OP_CHAN ? subop : line, n_arg);

	if (op == PLIX_OP_CHAN) {
		// spec: channel commands are IN-only; be lenient about direction
		return plix_cmd_chan(plix, subop, r_arg);
	}

	switch (op) {
		case PLIX_OP_GENERAL:
			if (dir == IO_OU) {
				// testuj - no real self-test to run, just say it passed
				plix_int_push(plix, PLIX_IWYTE, 0);
			}
			// dir == IO_IN: cofnij przerwanie (requeue unacked interrupt) -
			// not modeled (nothing to requeue in this stub), ack only
			return IO_OK;

		case PLIX_OP_SETCFG:
			// no packages/lines are actually parsed in this stub - just
			// accept whatever configuration was sent
			LOG(L_PLIX, "PLIX %i: SETCFG (stub: accepted, not parsed)", chan->num);
			plix_int_push(plix, PLIX_IUKON, 0);
			return IO_OK;

		case PLIX_OP_ATTACH:
			if (dir == IO_OU) {
				plix_int_push(plix, PLIX_IDOLI, line); // dołącz linię
			} else {
				plix_int_push(plix, PLIX_IODLI, line); // odłącz linię
			}
			return IO_OK;

		case PLIX_OP_STATUS:
			if (dir == IO_OU) {
				plix_int_push(plix, PLIX_ISTRE, line); // podaj status
			} else {
				plix_int_push(plix, PLIX_IABTR, line); // zerwij transmisję
			}
			return IO_OK;

		case PLIX_OP_XMIT:
			if (dir == IO_OU) {
				// przesyłaj - no device behind the line yet, report success
				// with nothing transmitted
				plix_int_push(plix, PLIX_IETRA, line);
			} else {
				plix_int_push(plix, PLIX_IZURZ, line); // zeruj urządzenie
			}
			return IO_OK;
	}

	return IO_OK;
}

// -----------------------------------------------------------------------
void plix_reset(chan_t *chan)
{
	chan_plix_t *plix = (chan_plix_t *) chan;
	elst_clear(plix->intq);
	LOG(L_PLIX, "PLIX %i: reset", chan->num);
}

// -----------------------------------------------------------------------
void plix_shutdown(chan_t *chan)
{
	if (!chan) return;
	chan_plix_t *plix = (chan_plix_t *) chan;

	LOG(L_PLIX, "PLIX %i: shutdown", chan->num);
	elst_destroy(plix->intq);
	free(plix);
}

// -----------------------------------------------------------------------
static int plix_connect_dev(chan_t *chan, int devnum, em400_dev_t *dev)
{
	(void) chan; (void) devnum; (void) dev;
	// unreachable while plix_dev_compatible() always returns false
	return E_ERR;
}

// -----------------------------------------------------------------------
chan_t * plix_create(int chnum)
{
	chan_plix_t *plix = (chan_plix_t *) calloc(1, sizeof(chan_plix_t));
	if (!plix) {
		LOGERR("Channel %i: memory allocation error.", chnum);
		return NULL;
	}

	plix->base.num = chnum;
	plix->base.type = EM400_CHANNEL_PLIX;
	plix->base.cmd = plix_cmd;
	plix->base.reset = plix_reset;
	plix->base.shutdown = plix_shutdown;
	plix->base.connect_dev = plix_connect_dev;

	plix->intq = elst_create(64, plix_int_destructor);
	if (!plix->intq) {
		free(plix);
		return NULL;
	}

	LOG(L_PLIX, "PLIX %i: channel created", chnum);

	return (chan_t *) plix;
}

// vim: tabstop=4 shiftwidth=4 autoindent
