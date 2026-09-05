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

#ifndef SP45DE_H
#define SP45DE_H

#include <inttypes.h>
#include <stdio.h>
#include <pthread.h>

#include "io/dev/dev.h"
#include "io/dev/sp45de_dir.h"
#include "io/dev/sp45de_crkdir.h"
#include "libem400.h"

#define SP45DE_TRACK_CNT 77			// physical tracks, numbered 0..76
#define SP45DE_TRACK_LAST 73		// last track of the working area (74..76 are spare)
#define SP45DE_SECTOR_PER_TRACK 26
#define SP45DE_BLK_SIZE 128

enum sp45de_buf_state {
	SP45DE_BUF_OK, SP45DE_BUF_END
};

// Result of a sector transfer. The physical medium's geometry is SP45DE's
// to enforce (the formatter/drive cannot address a track past the pack or
// a sector outside the track); UZFX only maps the result to a character-
// channel interrupt spec.
enum sp45de_result {
	SP45DE_R_OK = 0,		// sector transferred
	SP45DE_R_MEDIA_END,		// sector transferred, and it was the last of the working area
	SP45DE_R_NO_SECTOR,		// (track, sector) outside the physical medium - nothing transferred
	SP45DE_R_FAULT,			// no diskette in the slot, or a backend I/O error - nothing transferred
};

// True if (track, sector) is a physically addressable location on the medium.
bool sp45de_geometry_valid(unsigned track, unsigned sector);
// True if (track, sector) is the last addressable sector of the working area.
bool sp45de_at_media_end(unsigned track, unsigned sector);

typedef struct sp45de sp45de_t;

struct sp45de {
	struct em400_dev base;

	pthread_mutex_t media_mutex;
	char *image_name[EM400_SP45DE_SLOT_COUNT];
	FILE *image[EM400_SP45DE_SLOT_COUNT];
	sp45de_dir_t *dir[EM400_SP45DE_SLOT_COUNT];	// directory-backed slot: host dir as a raw IBM 3740 diskette
	sp45de_crkdir_t *crkdir[EM400_SP45DE_SLOT_COUNT];	// directory-backed slot: host dir as a CROOK-5 FS diskette (dir has .crookfs.ini)
	bool doors_locked;
	uint8_t buf[SP45DE_BLK_SIZE];
	unsigned buf_pos;
};

em400_dev_t * sp45de_create();
int sp45de_blk_read(sp45de_t *sp45de, unsigned slot, unsigned track, unsigned sector);
int sp45de_blk_write(sp45de_t *sp45de, unsigned slot, unsigned track, unsigned sector);
int sp45de_read(sp45de_t *sp45de, uint8_t *c);
int sp45de_write(sp45de_t *sp45de, uint8_t c);
int sp45de_motor_start(sp45de_t *sp45de);
int sp45de_motor_stop(sp45de_t *sp45de);


#endif

// vim: tabstop=4 shiftwidth=4 autoindent
