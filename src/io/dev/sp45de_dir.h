//  Copyright (c) 2026 Jakub Filipowicz <jakubf@gmail.com>
//  Copyright (c) 2026 Marcin Golesz <marcingolesz@gmail.com>
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

#ifndef SP45DE_DIR_H
#define SP45DE_DIR_H

// Directory-backed SP45DE 8" floppy: presents a host directory as an
// IBM 3740 / ISO 6596 basic-data-exchange diskette (77 tracks x 26 sectors x
// 128 bytes, single-sided). Track 0 is the index (ERMAP, VOL1, HDR1 records);
// files occupy contiguous extents on tracks 1..76. Changes flow both ways.
//
// This is the diskette's *physical* format, for raw 3740 data exchange with
// other systems and for loading standalone programs over the character channel.
// It is NOT a CROOK-5 filesystem - for a host directory CROOK-5 can attach and
// read/write, use sp45de_crkdir (selected when the directory holds a
// ".crookfs.ini").

#include <stdint.h>
#include <stdbool.h>

#include "io/dev/sp45de_geom.h"

typedef struct sp45de_dir sp45de_dir_t;

sp45de_dir_t * sp45de_dir_create(const char *dir_name, unsigned tracks,
                                 unsigned data_last, unsigned spt, unsigned blk_size);
void sp45de_dir_destroy(sp45de_dir_t *sd);

// track 0..tracks-1, sector 1..spt ; buf is blk_size bytes. Returns 0 on ok.
enum sp45de_result sp45de_dir_blk_rd(sp45de_dir_t *sd, unsigned track, unsigned sector, uint8_t *buf);
enum sp45de_result sp45de_dir_blk_wr(sp45de_dir_t *sd, unsigned track, unsigned sector, const uint8_t *buf);

#endif

// vim: tabstop=4 shiftwidth=4 autoindent
