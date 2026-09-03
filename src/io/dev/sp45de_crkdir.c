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
#include <time.h>

#include "log.h"
#include "io/dev/dev.h"
#include "io/dev/crook5fs.h"
#include "io/dev/sp45de_crkdir.h"

#define FSMETA_SIDECAR ".crookfs.ini"

// CROOK-5 FLOP8 filesystem geometry (validated against a BOSS `CFA 2 FLP 8 3 7
// 480` reference image):
//   baza A0 = 8, A1 = 17, A2 = 24, A3 = 25, AK = 480. Area name "FLP".
//   layout (logical 512-byte sectors):
//     0        LABEL
//     1-7      LOADER.FLP (reserved, zero)
//     8-16     DICDIC        (LABEL mirrored in sec 8/9/10 words 0-7; LIBRAR, BOSS)
//     17-23    FILDIC        (8 system pseudo-files; 4/2/1 hash cascade)
//     24       MAP.FLP       (480-bit allocation bitmap, sectors 0-58 allocated)
//     25-41    CDIREC.FLP    (recovery copy of the DICDIC sectors)
//     42-58    NDIREC.FLP    (reserved, zero)
//     59-479   free
#define FLP_A0   8
#define FLP_A1   17
#define FLP_A2   24
#define FLP_A3   25
#define FLP_AK   480
#define FLP_A3_FLAG 0x8000                     // LABEL[4] carries this flag
#define FLP_STRUCT_END 59                      // sectors 0..58 stay allocated
#define FLP_LOADER_LO   1
#define FLP_CDIREC_LO   25
#define FLP_CDIREC_HI   42
#define FLP_NDIREC_HI   59
#define BOSS_CODE  C5FS_DICDIC_LIBRAR_CODE      // set below to BOSS's own entry code

struct sp45de_crkdir {
	uint8_t *mem;                 // flat 128-byte-physical-sector image
	long size;
	unsigned tracks, spt, blk;
	c5fs_t *fs;
};

// ---------------------------------------------------------------------------
// filesystem synthesis
// ---------------------------------------------------------------------------

// one 12-word DICDIC entry, split 4/4/4 across logical sectors 8, 9, 10 at the
// same word offset (CROOK layout)
static void dicdic_entry(c5fs_t *fs, unsigned woff, const uint16_t e[12])
{
	for (unsigned part = 0 ; part < 3 ; part++) {
		uint8_t *sec = c5fs_lsec(fs, FLP_A0 + part);
		for (unsigned k = 0 ; k < 4 ; k++) c5fs_wrw(sec, woff + k, e[part * 4 + k]);
	}
}

// one FILDIC system pseudo-file: build the 12-word entry and hash-insert it
static void fildic_sys(struct c5_area *a, const char *name, uint16_t w6, uint16_t w8,
                       uint16_t start, uint16_t end, uint16_t len)
{
	uint16_t ename[2] = {0, 0};
	c5fs_r40_pack("FLP", 3, ename);
	uint16_t nm[2] = {0, 0};
	c5fs_r40_pack(name, C5FS_MAX_NAME, nm);

	uint16_t e[C5FS_FILDIC_ENTRY_WORDS] = {0};
	e[0] = nm[0]; e[1] = nm[1];
	e[2] = 0x30;                      // did = BOSS's code (48)
	e[3] = ename[0];                  // ext = area name "FLP"
	e[6] = w6;                        // 0x8000 static / 0xc000 recovery-region file
	e[7] = 4;                         // creator flags
	e[8] = w8;                        // 0 or 0xff43 (NDIREC / LOADER)
	e[9] = start; e[10] = end; e[11] = len;
	c5fs_fildic_insert(a, e);
}

static bool synth_flop8(sp45de_crkdir_t *sd)
{
	c5fs_t *fs = sd->fs;
	memset(sd->mem, 0, sd->size);

	uint16_t ename[2] = {0, 0};
	c5fs_r40_pack("FLP", 3, ename);

	// --- LABEL @ logical sector 0 ---
	time_t now = time(NULL);
	struct tm tmv;
	localtime_r(&now, &tmv);
	uint16_t label[17] = {0};
	label[0] = ename[0];
	label[1] = FLP_A0;
	label[2] = FLP_A1;
	label[3] = FLP_A2;
	label[4] = FLP_A3 | FLP_A3_FLAG;
	label[5] = FLP_AK;
	label[8]  = label[11] = (uint16_t)(tmv.tm_year + 1900);
	label[9]  = label[12] = (uint16_t)(tmv.tm_mon + 1);
	label[10] = label[13] = (uint16_t)tmv.tm_mday;
	label[14] = (uint16_t)tmv.tm_hour;
	label[15] = (uint16_t)tmv.tm_min;
	{
		uint8_t *l = c5fs_lsec(fs, 0);
		for (unsigned w = 0 ; w < 17 ; w++) c5fs_wrw(l, w, label[w]);
	}

	// --- DICDIC @ 8..16: LABEL[0..7] mirrored in sectors 8, 9, 10 ---
	for (unsigned s = 0 ; s < 3 ; s++) {
		uint8_t *sec = c5fs_lsec(fs, FLP_A0 + s);
		for (unsigned w = 0 ; w < 8 ; w++) c5fs_wrw(sec, w, label[w]);
	}
	// users: LIBRAR (word 8) and BOSS (word 12, child of LIBRAR)
	uint16_t librar[12] = {0}, boss[12] = {0};
	c5fs_r40_pack("LIBRAR", 6, librar);
	librar[3] = 1;             // one subdir (BOSS)
	librar[5] = 0x7fff;        // budget
	librar[7] = 0x7fff;        // rights
	c5fs_r40_pack("BOSS", 6, boss);
	boss[2] = C5FS_DICDIC_LIBRAR_CODE;        // parent = LIBRAR (code 32)
	boss[5] = 0x7fff;
	boss[6] = C5FS_DICDIC_LIBRAR_CODE;
	boss[7] = 0x7fff;
	dicdic_entry(fs, C5FS_DICDIC_LIBRAR_WOFF, librar);
	dicdic_entry(fs, C5FS_DICDIC_LIBRAR_WOFF + 4, boss);
	{ uint16_t end[12] = { 1, 0,0,0,0,0,0,0,0,0,0,0 };
	  dicdic_entry(fs, C5FS_DICDIC_LIBRAR_WOFF + 8, end); }

	// --- FILDIC @ 17..23: seed each sector's end marker + hash tail ---
	for (unsigned rel = 0 ; rel < FLP_A2 - FLP_A1 ; rel++) {
		uint8_t *sec = c5fs_lsec(fs, FLP_A1 + rel);
		uint16_t flag = 0, link = 0, mask = 0, idx = 0;
		if (rel < 4)          { link = (uint16_t)(0x15 + (rel & 1)); mask = 3; idx = (uint16_t)rel; }
		else if (rel < 6)     { link = 0x17;                         mask = 1; idx = (uint16_t)(rel - 4); }
		else                  { flag = 0xffff; }
		c5fs_wrw(sec, 0, 1);                       // end-of-dictionary marker
		c5fs_wrw(sec, 252, flag);
		c5fs_wrw(sec, 253, link);
		c5fs_wrw(sec, 254, mask);
		c5fs_wrw(sec, 255, idx);
	}

	struct c5_area a;
	if (!c5fs_area_open(&a, fs, 0)) return false;

	// the 8 system pseudo-files, in the order BOSS's CFA creates them
	fildic_sys(&a, "FILDIC", 0x8000, 0,      FLP_A1, FLP_A2,       (uint16_t)(FLP_A2 - FLP_A1));
	fildic_sys(&a, "CDIREC", 0xc000, 0,      FLP_CDIREC_LO, FLP_CDIREC_HI, (uint16_t)(FLP_CDIREC_HI - FLP_CDIREC_LO));
	fildic_sys(&a, "GLOBAL", 0x8000, 0,      0, FLP_AK,            FLP_AK);
	fildic_sys(&a, "MAP",    0x8000, 0,      FLP_A2, FLP_A3,       (uint16_t)(FLP_A3 - FLP_A2));
	fildic_sys(&a, "NDIREC", 0xc000, 0xff43, FLP_CDIREC_HI, FLP_NDIREC_HI, (uint16_t)(FLP_NDIREC_HI - FLP_CDIREC_HI));
	fildic_sys(&a, "LABEL",  0x8000, 0,      0, 1,                1);
	fildic_sys(&a, "DICDIC", 0x8000, 0,      FLP_A0, FLP_A1,      (uint16_t)(FLP_A1 - FLP_A0));
	fildic_sys(&a, "LOADER", 0xc000, 0xff43, FLP_LOADER_LO, FLP_A0, (uint16_t)(FLP_A0 - FLP_LOADER_LO));

	// --- MAPA @ 24: system sectors 0..58 allocated; sectors >= AK marked used
	// so they're never handed out (bits 480.. -> 0xffff padding to sector end) ---
	for (unsigned s = 0 ; s < FLP_STRUCT_END ; s++) c5fs_map_set(&a, s);
	{
		uint8_t *m = c5fs_lsec(fs, FLP_A2);
		for (unsigned w = (FLP_AK + 15) / 16 ; w < C5FS_SSIZE / 2 ; w++) c5fs_wrw(m, w, 0xffff);
	}

	// --- CDIREC.FLP @ 25..41: recovery copy of DICDIC + FILDIC + MAPA
	// (logical sectors A0..A3), 17 sectors ---
	for (unsigned s = 0 ; s < FLP_A3 - FLP_A0 ; s++)
		memcpy(c5fs_lsec(fs, FLP_CDIREC_LO + s), c5fs_lsec(fs, FLP_A0 + s), C5FS_SSIZE);

	LOG(L_FLOP, "sp45de crkdir: synthesized CROOK-5 FLOP8 area (A0=%u A1=%u A2=%u A3=%u AK=%u)",
		FLP_A0, FLP_A1, FLP_A2, FLP_A3, FLP_AK);
	return true;
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

sp45de_crkdir_t * sp45de_crkdir_create(const char *dir_name, unsigned tracks,
                                       unsigned spt, unsigned blk_size)
{
	sp45de_crkdir_t *sd = calloc(1, sizeof(*sd));
	if (!sd) return NULL;

	sd->tracks = tracks;
	sd->spt = spt;
	sd->blk = blk_size;
	sd->size = (long)tracks * spt * blk_size;

	sd->mem = malloc(sd->size);
	if (!sd->mem) {
		LOGERR("Failed to allocate %li bytes for the directory-backed SP45DE image.", sd->size);
		free(sd);
		return NULL;
	}

	sd->fs = c5fs_create(sd->mem, sd->size, 0, L_FLOP);
	if (!sd->fs) { free(sd->mem); free(sd); return NULL; }

	if (!synth_flop8(sd)) {
		LOGERR("Failed to synthesize the CROOK-5 FLOP8 area.");
		sp45de_crkdir_destroy(sd);
		return NULL;
	}

	if (dir_name && *dir_name)
		c5fs_mount_dir(sd->fs, dir_name, 0, FSMETA_SIDECAR);

	const char *dump = getenv("SP45DE_CRKDIR_DUMP");
	if (dump && *dump) {
		FILE *d = fopen(dump, "wb");
		if (d) { fwrite(sd->mem, 1, sd->size, d); fclose(d); }
	}

	return sd;
}

void sp45de_crkdir_destroy(sp45de_crkdir_t *sd)
{
	if (!sd) return;
	c5fs_flush(sd->fs);
	c5fs_destroy(sd->fs);
	free(sd->mem);
	free(sd);
}

static long blk_offset(const sp45de_crkdir_t *sd, unsigned track, unsigned sector)
{
	if (track >= sd->tracks || sector < 1 || sector > sd->spt) return -1;
	return ((long)track * sd->spt + (sector - 1)) * sd->blk;
}

int sp45de_crkdir_blk_rd(sp45de_crkdir_t *sd, unsigned track, unsigned sector, uint8_t *buf)
{
	long off = blk_offset(sd, track, sector);
	if (off < 0) return -1;
	c5fs_on_read(sd->fs);
	memcpy(buf, sd->mem + off, sd->blk);
	return 0;
}

int sp45de_crkdir_blk_wr(sp45de_crkdir_t *sd, unsigned track, unsigned sector, const uint8_t *buf)
{
	long off = blk_offset(sd, track, sector);
	if (off < 0) return -1;
	memcpy(sd->mem + off, buf, sd->blk);
	c5fs_on_write(sd->fs);
	return 0;
}

// vim: tabstop=4 shiftwidth=4 autoindent
