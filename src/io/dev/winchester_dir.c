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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "log.h"
#include "io/dev/dev.h"
#include "io/dev/crook5fs.h"
#include "io/dev/winchester_dir.h"

#define FSMETA_SIDECAR ".crookfs.ini"

// Directory-backed Winchester: the whole disk lives in RAM, either loaded from a
// base CROOK-5 image or synthesized empty. The host directory is mirrored into
// one data area's LIBRAR by the shared crook5fs engine.

struct winchester_dir {
	uint8_t *mem;
	long size;
	unsigned cyls, heads, spt, ssize;
	unsigned spare;                    // sectors hidden from CROOK (cylinder 0)
	bool variant_t;                    // CFA "T" - lay down the CDIREC recovery copy
	c5fs_t *fs;
};

// Peek the CFA "directory copy" answer out of <dir>/.crookfs.ini. CFA's own
// prompt is "create a file for the current copy of the directories (T|N)?";
// key `directory_copy`, value T/N (or Y/N), any line outside
// a [file] section. Default N.
static bool dir_wants_variant_t(const char *dir_name)
{
	if (!dir_name || !*dir_name) return false;
	char path[2048];
	snprintf(path, sizeof(path), "%s/.crookfs.ini", dir_name);
	FILE *f = fopen(path, "r");
	if (!f) return false;
	bool t = false;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '[') continue;
		if (strncasecmp(p, "directory_copy", 14)) continue;
		p = strchr(p, '=');
		if (!p) continue;
		do { p++; } while (*p == ' ' || *p == '\t');
		t = (*p == 'T' || *p == 't' || *p == 'Y' || *p == 'y');
		break;
	}
	fclose(f);
	return t;
}

// ---------------------------------------------------------------------------
// data-only synthesis: a fresh, empty CROOK-5 data area (A0 = 0) at logical 0,
// matching what BOSS's `CFA` produces for a plain data disk. Constants taken
// from area "D" of the reference image crook5-p8f-1.1.1.img:
//   A1=9 (9 DICDIC sectors), A2=109 (100 FILDIC sectors), A3=117 (8 MAPA),
//   AK=29472 (6 * 4912-sector quanta); NDIREC region A3..A3+117.
//   Users LIBRAR + BOSS; the 5 mandatory BOSS system pseudo-files
//   (MAP/DICDIC/GLOBAL/FILDIC/NDIREC) at their hash sectors; the per-sector
//   hash-overflow cascade (64/32/4) that CROOK validates on attach.
// ---------------------------------------------------------------------------

#define SYNTH_A1      9
#define SYNTH_A2      109
#define SYNTH_A3      117
#define SYNTH_AK      29472
#define SYNTH_NDIREC  117
#define SYNTH_BOSS_CODE   (12 * 4)     // BOSS is the 2nd DICDIC entry (word 12)
#define WINCH_QUANT   4912

static void synth_dicdic_entry(c5fs_t *fs, unsigned woff, const uint16_t e[12])
{
	for (unsigned part = 0 ; part < 3 ; part++) {
		uint8_t *sec = c5fs_lsec(fs, part);
		for (unsigned k = 0 ; k < 4 ; k++) c5fs_wrw(sec, woff + k, e[part * 4 + k]);
	}
}

static void synth_fildic_sys(c5fs_t *fs, unsigned rel, const char *name,
                             uint16_t ext_w, uint16_t w6,
                             uint16_t start, uint16_t end, uint16_t len)
{
	uint8_t *sec = c5fs_lsec(fs, SYNTH_A1 + rel);
	uint16_t nm[2] = {0, 0};
	c5fs_r40_pack(name, C5FS_MAX_NAME, nm);
	uint16_t e[C5FS_FILDIC_ENTRY_WORDS] = {0};
	e[0] = nm[0]; e[1] = nm[1];
	e[2] = SYNTH_BOSS_CODE;
	e[3] = ext_w;
	e[6] = w6;
	e[7] = 4;
	e[9] = start; e[10] = end; e[11] = len;
	for (unsigned k = 0 ; k < C5FS_FILDIC_ENTRY_WORDS ; k++)
		c5fs_wrw(sec, 0 * C5FS_FILDIC_ENTRY_WORDS + k, e[k]);
	c5fs_wrw(sec, 1 * C5FS_FILDIC_ENTRY_WORDS, 1);
}

static bool synth_data_area(winchester_dir_t *wd)
{
	unsigned usable = wd->cyls * wd->heads * wd->spt - wd->spare;
	if (usable <= SYNTH_AK) return false;

	const uint16_t A1 = SYNTH_A1, A2 = SYNTH_A2, A3 = SYNTH_A3, AK = SYNTH_AK;
	const bool t = wd->variant_t;
	// N: reserved NDIREC region A3..A3+117. T: a CDIREC recovery copy of
	// DICDIC+FILDIC+MAP (sectors 0..A3-1) takes that slot and NDIREC shifts
	// up by 117, and LABEL word 4 carries the 0x8000 flag.
	const unsigned struct_end = A3 + (t ? 2u : 1u) * SYNTH_NDIREC;

	memset(wd->mem, 0, wd->size);
	c5fs_t *fs = wd->fs;

	uint16_t ename[2] = {0, 0};
	c5fs_r40_pack("DAT", 3, ename);

	uint16_t meta[8] = { ename[0], 0 /*A0*/, A1, A2,
	                     (uint16_t)(t ? (A3 | 0x8000) : A3), AK, 0, 0 };
	for (unsigned s = 0 ; s < 3 ; s++) {
		uint8_t *sec = c5fs_lsec(fs, s);
		for (unsigned w = 0 ; w < 8 ; w++) c5fs_wrw(sec, w, meta[w]);
	}

	uint16_t librar[12] = {0}, boss[12] = {0};
	c5fs_r40_pack("LIBRAR", 6, librar);
	librar[3] = 1;
	librar[5] = 0x7fff;
	librar[7] = 0x7fff;
	c5fs_r40_pack("BOSS", 6, boss);
	boss[2] = C5FS_DICDIC_LIBRAR_CODE;
	boss[5] = 0x7fff;
	boss[6] = C5FS_DICDIC_LIBRAR_CODE;
	boss[7] = 0x7fff;
	synth_dicdic_entry(fs, C5FS_DICDIC_LIBRAR_WOFF, librar);
	synth_dicdic_entry(fs, C5FS_DICDIC_LIBRAR_WOFF + 4, boss);
	{ uint16_t end[12] = { 1, 0,0,0,0,0,0,0,0,0,0,0 };
	  synth_dicdic_entry(fs, C5FS_DICDIC_LIBRAR_WOFF + 8, end); }

	for (unsigned rel = 0 ; rel < (unsigned)(A2 - A1) ; rel++) {
		uint8_t *sec = c5fs_lsec(fs, A1 + rel);
		uint16_t link, mask, idx;
		if (rel < 64)       { link = (uint16_t)(A1 + 64 + (rel % 32));      mask = 63; idx = (uint16_t)rel; }
		else if (rel < 96)  { link = (uint16_t)(A1 + 96 + ((rel - 64) % 4)); mask = 31; idx = (uint16_t)(rel - 64); }
		else                { link = 0;                                     mask = 3;  idx = (uint16_t)(rel - 96); }
		c5fs_wrw(sec, 0, 1);
		c5fs_wrw(sec, 252, (rel == (unsigned)(A2 - A1) - 1) ? 0xffff : 0);
		c5fs_wrw(sec, 253, link);
		c5fs_wrw(sec, 254, mask);
		c5fs_wrw(sec, 255, idx);
	}

	const uint16_t ndirec = t ? (uint16_t)(A3 + SYNTH_NDIREC) : A3;

	synth_fildic_sys(fs,  9, "MAP",    ename[0], 0x8000, A2, A3, (uint16_t)(A3 - A2));
	synth_fildic_sys(fs, 10, "DICDIC", ename[0], 0x8000, 0,  A1, A1);
	synth_fildic_sys(fs, 29, "GLOBAL", ename[0], 0x8000, 0,  AK, AK);
	synth_fildic_sys(fs, 32, "FILDIC", ename[0], 0x8000, A1, A2, (uint16_t)(A2 - A1));
	if (t)
		synth_fildic_sys(fs, 56, "CDIREC", ename[0], 0xc000, A3, (uint16_t)(A3 + SYNTH_NDIREC), SYNTH_NDIREC);
	synth_fildic_sys(fs, 61, "NDIREC", ename[0], 0xc000, ndirec, (uint16_t)(ndirec + SYNTH_NDIREC), SYNTH_NDIREC);

	struct c5_area a;
	if (!c5fs_area_open(&a, fs, 0)) return false;
	for (unsigned s = 0 ; s < struct_end ; s++) c5fs_map_set(&a, s);
	// MAP bits past the last real sector (AK) read as allocated, like CFA
	for (unsigned s = AK ; s < (unsigned)(A3 - A2) * wd->ssize * 8 ; s++) c5fs_map_set(&a, s);

	if (t)
		// CDIREC.DAT = a copy of DICDIC + FILDIC + MAP (area sectors 0..A3-1)
		memcpy(c5fs_lsec(fs, A3), c5fs_lsec(fs, 0), (size_t)A3 * wd->ssize);

	LOG(L_WNCH, "winchester dir: synthesized empty CROOK-5 data area (A1=%u A2=%u A3=%u AK=%u)",
		A1, A2, A3, AK);
	return true;
}

// pick which CROOK area to overlay into: the first quantum-aligned *data* area
// (A0 == 0) that has a LIBRAR - so a bootable base image keeps its system area
// and the files land on the data disk. Falls back to logical 0.
static unsigned pick_overlay_area(winchester_dir_t *wd)
{
	unsigned usable = wd->cyls * wd->heads * wd->spt - wd->spare;
	for (unsigned start = 0 ; start + WINCH_QUANT <= usable ; start += WINCH_QUANT) {
		struct c5_area a;
		if (!c5fs_area_open(&a, wd->fs, start)) continue;
		if (a.A0 != 0) continue;
		if (!c5fs_librar_present(&a)) continue;
		if (start != 0)
			LOG(L_WNCH, "winchester dir: overlaying into data area at logical sector %u", start);
		return start;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

static long wd_offset(const winchester_dir_t *wd, unsigned c, unsigned h, unsigned s)
{
	if ((c >= wd->cyls) || (h >= wd->heads) || (s >= wd->spt)) return -1;
	long block = s + (h * wd->spt) + (c * wd->heads * wd->spt);
	return block * wd->ssize;
}

// -----------------------------------------------------------------------
winchester_dir_t * winchester_dir_create(const char *image_name, const char *dir_name,
                                         unsigned cyls, unsigned heads, unsigned spt,
                                         unsigned sector_size)
{
	winchester_dir_t *wd = calloc(1, sizeof(*wd));
	if (!wd) return NULL;

	wd->cyls = cyls;
	wd->heads = heads;
	wd->spt = spt;
	wd->ssize = sector_size;
	wd->spare = heads * spt;                        // cylinder 0
	wd->size = (long)cyls * heads * spt * sector_size;
	wd->variant_t = dir_wants_variant_t(dir_name);

	wd->mem = malloc(wd->size);
	if (!wd->mem) {
		LOGERR("Failed to allocate %li bytes for the directory-backed winchester image.", wd->size);
		winchester_dir_destroy(wd);
		return NULL;
	}

	wd->fs = c5fs_create(wd->mem, wd->size, wd->spare, L_WNCH);
	if (!wd->fs) { winchester_dir_destroy(wd); return NULL; }

	if (image_name && *image_name) {
		FILE *f = fopen(image_name, "rb");
		if (!f) {
			LOGERR("Failed to open base winchester image \"%s\".", image_name);
			winchester_dir_destroy(wd);
			return NULL;
		}
		if (fseek(f, 0, SEEK_END) || (ftell(f) != wd->size)) {
			LOGERR("Base winchester image \"%s\" size does not match the %u/%u/%u geometry (expected %li bytes).",
				image_name, cyls, heads, spt, wd->size);
			fclose(f);
			winchester_dir_destroy(wd);
			return NULL;
		}
		rewind(f);
		if (fread(wd->mem, 1, wd->size, f) != (size_t)wd->size) {
			LOGERR("Failed to read base winchester image \"%s\".", image_name);
			fclose(f);
			winchester_dir_destroy(wd);
			return NULL;
		}
		fclose(f);
		LOG(L_WNCH, "Directory-backed winchester: base image \"%s\" (%li bytes) loaded into RAM", image_name, wd->size);
	} else {
		if (!synth_data_area(wd)) {
			LOGERR("Failed to synthesize the data-only winchester area.");
			winchester_dir_destroy(wd);
			return NULL;
		}
	}

	if (dir_name && *dir_name)
		c5fs_mount_dir(wd->fs, dir_name, pick_overlay_area(wd), FSMETA_SIDECAR);

	const char *dump = getenv("WINCH_DIR_DUMP");
	if (dump && *dump) {
		FILE *d = fopen(dump, "wb");
		if (d) { fwrite(wd->mem, 1, wd->size, d); fclose(d); }
	}

	return wd;
}

// -----------------------------------------------------------------------
void winchester_dir_destroy(winchester_dir_t *wd)
{
	if (!wd) return;
	c5fs_flush(wd->fs);
	c5fs_destroy(wd->fs);
	free(wd->mem);
	free(wd);
}

// -----------------------------------------------------------------------
int winchester_dir_sector_rd(winchester_dir_t *wd, uint8_t *buf, unsigned c, unsigned h, unsigned s)
{
	long off = wd_offset(wd, c, h, s);
	if (off < 0) return DEV_STATUS_SEEKERR;
	c5fs_on_read(wd->fs);
	memcpy(buf, wd->mem + off, wd->ssize);
	return DEV_STATUS_OK;
}

// -----------------------------------------------------------------------
int winchester_dir_sector_wr(winchester_dir_t *wd, uint8_t *buf, unsigned c, unsigned h, unsigned s)
{
	long off = wd_offset(wd, c, h, s);
	if (off < 0) return DEV_STATUS_SEEKERR;
	memcpy(wd->mem + off, buf, wd->ssize);
	c5fs_on_write(wd->fs);
	return DEV_STATUS_OK;
}

// vim: tabstop=4 shiftwidth=4 autoindent
