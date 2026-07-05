//  Copyright (c) 2025 Jakub Filipowicz <jakubf@gmail.com>
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

#include <stdlib.h>
#include <string.h>

#include "log.h"

#include "io/dev/winchester.h"

#define WINCH_CYLS 615
#define WINCH_HEADS 4
#define WINCH_SPT 16
#define WINCH_SECTOR_SIZE 512

// -----------------------------------------------------------------------
static long winchester_offset(unsigned c, unsigned h, unsigned s)
{
	if ((c >= WINCH_CYLS) || (h >= WINCH_HEADS) || (s >= WINCH_SPT)) return -1;
	long block = s + (h * WINCH_SPT) + (c * WINCH_HEADS * WINCH_SPT);
	return block * WINCH_SECTOR_SIZE;
}

// -----------------------------------------------------------------------
int winchester_sector_rd(winchester_t *winchester, uint8_t *buf, unsigned c, unsigned h, unsigned s)
{
	if (!winchester->image) return DEV_STATUS_NOMEDIUM;

	long offset = winchester_offset(c, h, s);
	if ((offset < 0) || fseek(winchester->image, offset, SEEK_SET)) return DEV_STATUS_SEEKERR;
	if (fread(buf, 1, WINCH_SECTOR_SIZE, winchester->image) != WINCH_SECTOR_SIZE) return DEV_STATUS_RDERR;

	return DEV_STATUS_OK;
}

// -----------------------------------------------------------------------
int winchester_sector_wr(winchester_t *winchester, uint8_t *buf, unsigned c, unsigned h, unsigned s)
{
	if (!winchester->image) return DEV_STATUS_NOMEDIUM;

	long offset = winchester_offset(c, h, s);
	if ((offset < 0) || fseek(winchester->image, offset, SEEK_SET)) return DEV_STATUS_SEEKERR;
	if (fwrite(buf, 1, WINCH_SECTOR_SIZE, winchester->image) != WINCH_SECTOR_SIZE) return DEV_STATUS_WRERR;

	return DEV_STATUS_OK;
}

// -----------------------------------------------------------------------
bool winchester_ready(em400_dev_t *dev)
{
	return dev && ((winchester_t *) dev)->image != NULL;
}

// -----------------------------------------------------------------------
static void winchester_ioloop_teardown(winchester_t * winchester)
{

}

// -----------------------------------------------------------------------
void winchester_shutdown(em400_dev_t *dev)
{
	if (!dev) return;
	winchester_t *winchester = (winchester_t *) dev;

	LOG(L_WNCH, "Winchester shutting down");

	winchester_ioloop_teardown(winchester);
	if (winchester->image) fclose(winchester->image);
	free(winchester->image_name);
	free(winchester);
}

// -----------------------------------------------------------------------
void winchester_reset(em400_dev_t *dev)
{
	if (!dev) return;
	LOG(L_WNCH, "Winchester reset");
}

// -----------------------------------------------------------------------
bool winchester_can_eject(em400_dev_t *dev, unsigned slot)
{
	// non-removable media, set during initialization only
	return false;
}

// -----------------------------------------------------------------------
const char * winchester_image(em400_dev_t *dev, unsigned slot)
{
	winchester_t *winchester = (winchester_t *) dev;
	return winchester->image_name;
}

// -----------------------------------------------------------------------
em400_dev_t * winchester_create(const char *image_name)
{
	LOG(L_WNCH, "Creating winchester");

	winchester_t *winchester = calloc(1, sizeof(winchester_t));
	if (!winchester) {
		LOGERR("Failed to allocate memory for winchester device with image %s", image_name);
		goto fail;
	}

	winchester->base.type = EM400_DEV_WINCHESTER;
	winchester->base.slot_count = 1;
	winchester->base.reset = winchester_reset;
	winchester->base.write = NULL;
	winchester->base.shutdown = winchester_shutdown;

	winchester->base.can_eject = winchester_can_eject;
	winchester->base.load = NULL;
	winchester->base.eject = NULL;
	winchester->base.image = winchester_image;

	if (image_name && *image_name) {
		winchester->image_name = strdup(image_name);
		LOG(L_WNCH, "Opening image: %s", winchester->image_name);

		// Image set by the user but unavailable or wrong still allows the machine to boot
		// but: a) show user the error, b) disk is not operational
		winchester->image = fopen(winchester->image_name, "rb+");
		if (!winchester->image) {
			LOGWARN("Failed to open Winchester image \"%s\". Drive starts not ready (no winchester connected).", winchester->image_name);
		} else {
			long expected = (long) WINCH_CYLS * WINCH_HEADS * WINCH_SPT * WINCH_SECTOR_SIZE;
			if (fseek(winchester->image, 0, SEEK_END) || (ftell(winchester->image) != expected)) {
				LOGWARN("Winchester image \"%s\" size does not match the %i/%i/%i geometry (expected %li bytes). Drive starts not ready (no winchester connected).", winchester->image_name, WINCH_CYLS, WINCH_HEADS, WINCH_SPT, expected);
				fclose(winchester->image);
				winchester->image = NULL;
			}
		}
	} else {
		// No image set = no winchester drive, treated as intended by the user.
		// Machine boots, but winchester operations give errors.
		LOG(L_WNCH, "No image, winchester starts not ready (no winchester disk connected)");
	}

	return (em400_dev_t *) winchester;
fail:
	return NULL;
}

