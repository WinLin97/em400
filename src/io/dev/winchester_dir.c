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
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include "log.h"
#include "io/dev/dev.h"
#include "io/dev/crook5fs.h"
#include "io/dev/winchester_dir.h"

#define FSMETA_SIDECAR ".crookfs.ini"
#define WD_MAX_AREAS 8

// Directory-backed Winchester: the whole disk lives in RAM, either loaded from a
// base CROOK-5 image or synthesized empty. Each mounted host directory is
// mirrored into one CROOK-5 area's LIBRAR by the shared crook5fs engine. In
// synth mode the mount root's files become area "DAT" and each immediate
// subdirectory becomes its own area (named after the subdir, 3 R40 chars).

struct winchester_dir {
	uint8_t *mem;
	long size;
	unsigned cyls, heads, spt, ssize;
	unsigned spare;                    // sectors hidden from CROOK (cylinder 0)
	bool variant_t;                    // CFA "T" - lay down the CDIREC recovery copy
	c5fs_t *scratch;                   // whole-disk handle: synth writes + area_open
	unsigned narea;
	c5fs_t *area[WD_MAX_AREAS];        // one mounted crook5fs per CROOK-5 area
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

#define SYNTH_A1          9       // 9 DICDIC sectors (CFA pa1 = 3)
#define SYNTH_FILDIC_PER_QUANT 20 // FILDIC sectors per 4912-sector quantum
#define SYNTH_BOSS_CODE   (12 * 4)
#define WINCH_QUANT       4912

static void synth_dicdic_entry(c5fs_t *fs, unsigned base, unsigned woff, const uint16_t e[12])
{
	for (unsigned part = 0 ; part < 3 ; part++) {
		uint8_t *sec = c5fs_lsec(fs, base + part);
		for (unsigned k = 0 ; k < 4 ; k++) c5fs_wrw(sec, woff + k, e[part * 4 + k]);
	}
}

static void synth_fildic_sys(struct c5_area *a, const char *name,
                             uint16_t ext_w, uint16_t w6,
                             uint16_t start, uint16_t end, uint16_t len)
{
	uint16_t e[C5FS_FILDIC_ENTRY_WORDS] = {0};
	c5fs_r40_pack(name, C5FS_MAX_NAME, e);
	e[2] = SYNTH_BOSS_CODE;
	e[3] = ext_w;
	e[6] = w6;
	e[7] = 4;
	e[9] = start; e[10] = end; e[11] = len;
	c5fs_fildic_insert(a, e);   // places it at its hash sector, spills on collision
}

// The FILDIC per-sector hash cascade: level sizes are the descending powers of
// two that sum to `n` (100 -> 64+32+4, 20 -> 16+4, 7 -> 4+2+1); a sector in
// level L links, cycling, into level L+1. CROOK validates this on attach.
static void synth_cascade(c5fs_t *fs, unsigned base, unsigned A1, unsigned n)
{
	unsigned lvl[16], nlev = 0, rem = n;
	while (rem && nlev < 16) { unsigned k = 1; while (k * 2 <= rem) k *= 2; lvl[nlev++] = k; rem -= k; }
	unsigned lstart[17]; lstart[0] = 0;
	for (unsigned i = 0 ; i < nlev ; i++) lstart[i + 1] = lstart[i] + lvl[i];

	for (unsigned rel = 0 ; rel < n ; rel++) {
		unsigned L = 0;
		while (L + 1 < nlev && rel >= lstart[L + 1]) L++;
		unsigned idx = rel - lstart[L];
		unsigned link = (L + 1 < nlev) ? (A1 + lstart[L + 1] + (idx % lvl[L + 1])) : 0;
		uint8_t *sec = c5fs_lsec(fs, base + A1 + rel);
		c5fs_wrw(sec, 0, 1);   // end-of-dictionary marker in the first slot
		c5fs_wrw(sec, 252, (uint16_t)((rel == n - 1) ? 0xffff : 0));
		c5fs_wrw(sec, 253, (uint16_t) link);
		c5fs_wrw(sec, 254, (uint16_t)(lvl[L] - 1));
		c5fs_wrw(sec, 255, (uint16_t) idx);
	}
}

// Lay down one empty CROOK-5 data area (A0 = 0) of `quanta` quanta at logical
// sector `base`, named `name3`. Matches what BOSS's `CFA ,<name>,0,3,<pa2>,<pa3>`
// produces. Returns AK (the area's sector count) or 0 on failure.
static unsigned synth_one_area(c5fs_t *fs, unsigned base, unsigned ssize,
                               const char *name3, unsigned quanta, bool t)
{
	const unsigned AK = quanta * WINCH_QUANT;
	const unsigned A1 = SYNTH_A1;
	const unsigned A2 = A1 + SYNTH_FILDIC_PER_QUANT * quanta;
	const unsigned mapsec = (AK + ssize * 8 - 1) / (ssize * 8);
	const unsigned A3 = A2 + mapsec;
	const unsigned ndirec_len = A3;                     // NDIREC / CDIREC region = |metadata|
	const unsigned struct_end = A3 + (t ? 2u : 1u) * ndirec_len;
	if (struct_end >= AK) return 0;

	uint16_t ename[2] = {0, 0};
	c5fs_r40_pack(name3, C5FS_MAX_NAME, ename);

	uint16_t meta[8] = { ename[0], 0, (uint16_t) A1, (uint16_t) A2,
	                     (uint16_t)(t ? (A3 | 0x8000) : A3), (uint16_t) AK, 0, 0 };
	for (unsigned s = 0 ; s < 3 ; s++) {
		uint8_t *sec = c5fs_lsec(fs, base + s);
		for (unsigned w = 0 ; w < 8 ; w++) c5fs_wrw(sec, w, meta[w]);
	}

	uint16_t librar[12] = {0}, boss[12] = {0};
	c5fs_r40_pack("LIBRAR", 6, librar);
	librar[3] = 1; librar[5] = 0x7fff; librar[7] = 0x7fff;
	c5fs_r40_pack("BOSS", 6, boss);
	boss[2] = C5FS_DICDIC_LIBRAR_CODE; boss[5] = 0x7fff;
	boss[6] = C5FS_DICDIC_LIBRAR_CODE; boss[7] = 0x7fff;
	synth_dicdic_entry(fs, base, C5FS_DICDIC_LIBRAR_WOFF, librar);
	synth_dicdic_entry(fs, base, C5FS_DICDIC_LIBRAR_WOFF + 4, boss);
	{ uint16_t end[12] = { 1, 0,0,0,0,0,0,0,0,0,0,0 };
	  synth_dicdic_entry(fs, base, C5FS_DICDIC_LIBRAR_WOFF + 8, end); }

	synth_cascade(fs, base, A1, A2 - A1);

	struct c5_area a;
	if (!c5fs_area_open(&a, fs, base)) return 0;

	const unsigned ndirec = t ? A3 + ndirec_len : A3;
	synth_fildic_sys(&a, "MAP",    ename[0], 0x8000, (uint16_t) A2, (uint16_t) A3, (uint16_t) mapsec);
	synth_fildic_sys(&a, "DICDIC", ename[0], 0x8000, 0, (uint16_t) A1, (uint16_t) A1);
	synth_fildic_sys(&a, "GLOBAL", ename[0], 0x8000, 0, (uint16_t) AK, (uint16_t) AK);
	synth_fildic_sys(&a, "FILDIC", ename[0], 0x8000, (uint16_t) A1, (uint16_t) A2, (uint16_t)(A2 - A1));
	if (t)
		synth_fildic_sys(&a, "CDIREC", ename[0], 0xc000, (uint16_t) A3, (uint16_t)(A3 + ndirec_len), (uint16_t) ndirec_len);
	synth_fildic_sys(&a, "NDIREC", ename[0], 0xc000, (uint16_t) ndirec, (uint16_t)(ndirec + ndirec_len), (uint16_t) ndirec_len);

	for (unsigned s = 0 ; s < struct_end ; s++) c5fs_map_set(&a, s);
	for (unsigned s = AK ; s < mapsec * ssize * 8 ; s++) c5fs_map_set(&a, s);

	if (t)
		memcpy(c5fs_lsec(fs, base + A3), c5fs_lsec(fs, base), (size_t) A3 * ssize);

	LOG(L_WNCH, "winchester dir: synthesized area \"%s\" @ %u (A1=%u A2=%u A3=%u AK=%u, %s)",
		name3, base, A1, A2, A3, AK, t ? "T" : "N");
	return AK;
}

// pick which CROOK area to overlay into: the first quantum-aligned *data* area
// (A0 == 0) that has a LIBRAR - so a bootable base image keeps its system area
// and the files land on the data disk. Falls back to logical 0.
static unsigned pick_overlay_area(winchester_dir_t *wd)
{
	unsigned usable = wd->cyls * wd->heads * wd->spt - wd->spare;
	for (unsigned start = 0 ; start + WINCH_QUANT <= usable ; start += WINCH_QUANT) {
		struct c5_area a;
		if (!c5fs_area_open(&a, wd->scratch, start)) continue;
		if (a.A0 != 0) continue;
		if (!c5fs_librar_present(&a)) continue;
		if (start != 0)
			LOG(L_WNCH, "winchester dir: overlaying into data area at logical sector %u", start);
		return start;
	}
	return 0;
}

// One synthesised area: its 3-char CROOK name and the host directory feeding it.
struct wd_area_spec {
	char name[4];
	char path[1536];
};

// A host subdir name -> a 3-char R40 CROOK area name (upper, [A-Z0-9_%#] only).
static void area_name_from(const char *sub, char out[4])
{
	unsigned o = 0;
	for (const char *p = sub ; *p && o < 3 ; p++) {
		int c = toupper((unsigned char) *p);
		if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '%' || c == '#')
			out[o++] = (char) c;
	}
	if (o == 0) out[o++] = 'X';
	out[o] = 0;
}

// Enumerate the areas a synth mount produces: root files -> "DAT", each
// immediate subdirectory -> its own area. Returns the count (>=1).
static unsigned wd_collect_areas(const char *root, struct wd_area_spec *out, unsigned max)
{
	unsigned n = 0;
	bool root_files = false;
	DIR *d = opendir(root);
	if (d) {
		struct dirent *de;
		while ((de = readdir(d)) && n < max) {
			if (de->d_name[0] == '.') continue;
			char p[1536];
			snprintf(p, sizeof(p), "%s/%s", root, de->d_name);
			struct stat st;
			if (stat(p, &st)) continue;
			if (S_ISDIR(st.st_mode)) {
				area_name_from(de->d_name, out[n].name);
				snprintf(out[n].path, sizeof(out[n].path), "%s", p);
				n++;
			} else if (S_ISREG(st.st_mode)) {
				root_files = true;
			}
		}
		closedir(d);
	}
	if (root_files || n == 0) {
		// shift subdir areas up, put the root area first
		if (n < max) {
			for (unsigned i = n ; i > 0 ; i--) out[i] = out[i - 1];
			memcpy(out[0].name, "DAT", 4);
			snprintf(out[0].path, sizeof(out[0].path), "%s", root);
			n++;
		}
	}
	return n;
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

	wd->scratch = c5fs_create(wd->mem, wd->size, wd->spare, L_WNCH);
	if (!wd->scratch) { winchester_dir_destroy(wd); return NULL; }

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

		// overlay: one area, chosen from the existing image
		if (dir_name && *dir_name) {
			wd->area[0] = c5fs_create(wd->mem, wd->size, wd->spare, L_WNCH);
			if (wd->area[0]) {
				c5fs_mount_dir(wd->area[0], dir_name, pick_overlay_area(wd), FSMETA_SIDECAR);
				wd->narea = 1;
			}
		}
	} else {
		// synth: root files -> "DAT", each subdir -> its own area, quanta split evenly
		struct wd_area_spec spec[WD_MAX_AREAS];
		unsigned n = dir_name && *dir_name ? wd_collect_areas(dir_name, spec, WD_MAX_AREAS) : 0;
		if (n == 0) { memcpy(spec[0].name, "DAT", 4); spec[0].path[0] = 0; n = 1; }

		memset(wd->mem, 0, wd->size);
		unsigned usable = cyls * heads * spt - wd->spare;
		unsigned total_q = usable / WINCH_QUANT;
		unsigned per_q = total_q / n;
		if (per_q == 0) { LOGERR("winchester dir: %u areas do not fit in %u quanta.", n, total_q); winchester_dir_destroy(wd); return NULL; }

		unsigned base = 0;
		for (unsigned i = 0 ; i < n ; i++) {
			if (!synth_one_area(wd->scratch, base, wd->ssize, spec[i].name, per_q, wd->variant_t)) {
				LOGERR("winchester dir: failed to synthesize area \"%s\".", spec[i].name);
				winchester_dir_destroy(wd);
				return NULL;
			}
			wd->area[i] = c5fs_create(wd->mem, wd->size, wd->spare, L_WNCH);
			if (wd->area[i] && spec[i].path[0])
				c5fs_mount_dir(wd->area[i], spec[i].path, base, FSMETA_SIDECAR);
			base += per_q * WINCH_QUANT;
		}
		wd->narea = n;
	}

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
	for (unsigned i = 0 ; i < wd->narea ; i++) {
		if (!wd->area[i]) continue;
		c5fs_flush(wd->area[i]);
		c5fs_destroy(wd->area[i]);
	}
	if (wd->scratch) c5fs_destroy(wd->scratch);
	free(wd->mem);
	free(wd);
}

// -----------------------------------------------------------------------
int winchester_dir_sector_rd(winchester_dir_t *wd, uint8_t *buf, unsigned c, unsigned h, unsigned s)
{
	long off = wd_offset(wd, c, h, s);
	if (off < 0) return DEV_STATUS_SEEKERR;
	for (unsigned i = 0 ; i < wd->narea ; i++)
		if (wd->area[i]) c5fs_on_read(wd->area[i]);
	memcpy(buf, wd->mem + off, wd->ssize);
	return DEV_STATUS_OK;
}

// -----------------------------------------------------------------------
int winchester_dir_sector_wr(winchester_dir_t *wd, uint8_t *buf, unsigned c, unsigned h, unsigned s)
{
	long off = wd_offset(wd, c, h, s);
	if (off < 0) return DEV_STATUS_SEEKERR;
	memcpy(wd->mem + off, buf, wd->ssize);
	for (unsigned i = 0 ; i < wd->narea ; i++)
		if (wd->area[i]) c5fs_on_write(wd->area[i]);
	return DEV_STATUS_OK;
}

// vim: tabstop=4 shiftwidth=4 autoindent
