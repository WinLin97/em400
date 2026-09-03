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

#ifndef CROOK5FS_H
#define CROOK5FS_H

// Shared CROOK-5 filesystem core for the directory-backed disk devices
// (winchester_dir, sp45de_crkdir). It keeps the whole disk as an in-RAM image
// of 16-bit big-endian, 512-byte "logical" sectors, and mirrors a host
// directory into one CROOK-5 disk area's LIBRAR:
//   host  -> guest : polled (mtime/size), re-overlaid into the RAM image
//   guest -> host  : after CROOK settles a write, changed/created/deleted files
//                    under LIBRAR (not part of the synthesized/base structure)
//                    are written back to the host directory.
//
// The device-specific parts (physical geometry, the CFA-style area synthesis)
// live in the backend; this module owns the FS structures and the sync engine.

#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>

#define C5FS_SSIZE       512
#define C5FS_MAX_NAME    6
#define C5FS_MAX_EXT     3
#define C5FS_FILDIC_ENTRY_WORDS 12
#define C5FS_FILDIC_SLOTS       21
#define C5FS_DICDIC_LIBRAR_WOFF 8
#define C5FS_DICDIC_LIBRAR_CODE (C5FS_DICDIC_LIBRAR_WOFF * 4)

typedef struct c5fs c5fs_t;

// A CROOK-5 disk area located inside the RAM image. `A0..AK` are the LABEL
// pointers relative to the area start (A0 doubles as the metryka position when
// the area has no separate LABEL, i.e. A0 == 0).
struct c5_area {
	c5fs_t *fs;
	unsigned start;                    // area's logical start sector (image-relative)
	uint16_t A0, A1, A2, A3, AK;
	unsigned fildic_sectors;
};

// --- lifecycle ------------------------------------------------------------

// Create over a caller-owned RAM image of `size` bytes (512-byte logical
// sectors). `spare` logical sectors at the front are hidden from CROOK
// addressing (e.g. a winchester's cylinder 0). `logc` is the log component to
// emit messages on (L_WNCH / L_FLOP). Takes no ownership of `mem`.
c5fs_t * c5fs_create(uint8_t *mem, long size, unsigned spare, int logc);
void c5fs_destroy(c5fs_t *fs);

// Point the FS at a host directory and do the initial overlay into the area at
// `area_start` (which must already hold a valid CROOK-5 area with a LIBRAR).
// Enables two-way sync + the host-directory watcher. `sidecar` is the
// per-file-attributes filename (e.g. ".crookfs.ini").
void c5fs_mount_dir(c5fs_t *fs, const char *dir_name, unsigned area_start,
                    const char *sidecar);

// Final guest->host flush; called from the backend's destroy path before
// c5fs_destroy().
void c5fs_flush(c5fs_t *fs);

// --- sync hooks (call from the backend's sector I/O path) ----------------

void c5fs_on_read(c5fs_t *fs);     // may run a pending host->guest / guest->host sync
void c5fs_on_write(c5fs_t *fs);    // mark the image dirty (CROOK wrote a sector)

// --- low-level image access (for the backend's area synthesis) ----------

uint8_t * c5fs_lsec(c5fs_t *fs, unsigned lsec);          // -> 512 bytes
uint16_t  c5fs_rdw(const uint8_t *p, unsigned word);
void      c5fs_wrw(uint8_t *p, unsigned word, uint16_t v);
void      c5fs_r40_pack(const char *s, unsigned n, uint16_t *out);   // n chars -> ceil(n/3) words

// Open the area at `start`; fills `*a`. Returns false if the LABEL is invalid.
bool c5fs_area_open(struct c5_area *a, c5fs_t *fs, unsigned start);
uint8_t * c5fs_area_sec(struct c5_area *a, unsigned area_rel_sec);

// MAPA helpers (area-relative sector numbers)
void c5fs_map_set(struct c5_area *a, unsigned sec);
bool c5fs_map_get(struct c5_area *a, unsigned sec);

// Insert one 12-word FILDIC entry at its hash sector. Returns false if full.
bool c5fs_fildic_insert(struct c5_area *a, const uint16_t ent[C5FS_FILDIC_ENTRY_WORDS]);

// Compute the FILDIC hash sector for an R40 name.
unsigned c5fs_fildic_hash(struct c5_area *a, uint16_t w0, uint16_t w1);

// True if the area's DICDIC root entry is "LIBRAR".
bool c5fs_librar_present(struct c5_area *a);

#endif

// vim: tabstop=4 shiftwidth=4 autoindent
