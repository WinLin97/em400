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

#ifndef WINCHESTER_DIR_H
#define WINCHESTER_DIR_H

// Directory-backed Winchester: presents a host directory as a live CROOK-5
// filesystem, sitting behind the same CHS sector interface as a plain image.
// A DOSBox-style "mount" for the MERA-400 hard disk.
//
// The backend keeps the whole disk as an in-memory image: a base CROOK-5 image
// (for the boot / system area) with the host directory's files overlaid into the
// filesystem structures.

#include <stdint.h>
#include <stdbool.h>

typedef struct winchester_dir winchester_dir_t;

// Build the backend from a base CROOK-5 image and a host directory to mirror.
// Returns NULL on failure (missing/invalid image, unreadable directory).
winchester_dir_t * winchester_dir_create(const char *image_name, const char *dir_name,
                                         unsigned cyls, unsigned heads, unsigned spt,
                                         unsigned sector_size);

void winchester_dir_destroy(winchester_dir_t *wd);

// CHS sector access, same contract as winchester_sector_rd/wr
// (return values are enum dev_cmd_status).
int winchester_dir_sector_rd(winchester_dir_t *wd, uint8_t *buf, unsigned c, unsigned h, unsigned s);
int winchester_dir_sector_wr(winchester_dir_t *wd, uint8_t *buf, unsigned c, unsigned h, unsigned s);

#endif

// vim: tabstop=4 shiftwidth=4 autoindent
