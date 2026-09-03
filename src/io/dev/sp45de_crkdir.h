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

#ifndef SP45DE_CRKDIR_H
#define SP45DE_CRKDIR_H

// CROOK-5-filesystem-backed SP45DE 8" floppy: presents a host directory as a
// live CROOK-5 disk area on an 8" diskette. Unlike sp45de_dir (raw IBM 3740),
// this builds the CROOK-5 filesystem `CFA` would produce - baza 8, 480 logical
// 512-byte sectors (four 128-byte physical sectors each) - so CROOK-5 attaches
// it as a disk area (`LOD ,<n>`) and can list / read / write files on it.
//
// Selected when the host directory contains a ".crookfs.ini" file; otherwise
// sp45de_dir's IBM-3740 synthesizer is used.

#include <stdint.h>
#include <stdbool.h>

typedef struct sp45de_crkdir sp45de_crkdir_t;

sp45de_crkdir_t * sp45de_crkdir_create(const char *dir_name, unsigned tracks,
                                       unsigned spt, unsigned blk_size);
void sp45de_crkdir_destroy(sp45de_crkdir_t *sd);

// track 0..tracks-1, sector 1..spt ; buf is blk_size (128) bytes. 0 on ok.
int sp45de_crkdir_blk_rd(sp45de_crkdir_t *sd, unsigned track, unsigned sector, uint8_t *buf);
int sp45de_crkdir_blk_wr(sp45de_crkdir_t *sd, unsigned track, unsigned sector, const uint8_t *buf);

#endif

// vim: tabstop=4 shiftwidth=4 autoindent
