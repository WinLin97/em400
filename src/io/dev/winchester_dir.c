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
#include <ctype.h>
#include <time.h>
#include <stdatomic.h>
#include <dirent.h>
#include <sys/stat.h>

#include "log.h"
#include "io/dev/dev.h"
#include "io/dev/dirwatch.h"
#include "io/dev/fsmeta.h"
#include "io/dev/winchester_dir.h"

#define FSMETA_SIDECAR ".crookfs.ini"

// Directory-backed Winchester ("mount" a host directory as a CROOK-5 disk).
//
// The whole disk lives in RAM. Two ways to get the CROOK-5 structure:
//   - with a base image: load it, overlay the host dir into its first data area
//     (A0==0) that has a LIBRAR - a bootable image keeps its system area intact.
//   - without any image: synthesize a fresh, empty CROOK-5 data area from scratch
//     (synth_data_area(), no file dependency at all).
// The host files are overlaid under that area's LIBRAR. Changes flow both ways:
//   host  -> guest : polled (mtime/size), re-overlaid into the RAM image
//   guest -> host  : after CROOK finishes a write, changed/created/deleted
//                    files under LIBRAR (that aren't part of the base image)
//                    are written back to the host directory

#define MAX_NAME 6
#define MAX_EXT  3
#define FILDIC_ENTRY_WORDS 12
#define FILDIC_SLOTS 21
#define FILDIC_COUNTER_SECTOR 25       // FILDIC-relative sector holding the master
#define FILDIC_COUNTER_WORD   92       //  fs-wide file-creation counter
#define DICDIC_LIBRAR_WOFF    8        // first DICDIC entry (part 1) word offset
#define DICDIC_LIBRAR_CODE    (DICDIC_LIBRAR_WOFF * 4)

#define SYNC_QUIESCE_SECS  1           // wait this long after CROOK's last write
#define POLL_INTERVAL_SECS 2           // host directory rescan cadence
#define MAX_FILE_BYTES     (20L * 1024 * 1024)   // whole-disk ceiling; real limit is the free run

// one host file we injected / are tracking
struct tracked {
	char name[MAX_NAME + 1];
	char ext[MAX_EXT + 1];
	uint16_t w0, w1, ew;              // R40 name / ext words
	unsigned start, nsec;            // where its data lives in the RAM image (area-relative)
	long host_size;
	long host_mtime;
	uint32_t crc;                    // CRC32 of the file data in the image
};

struct winchester_dir {
	char *dir_name;
	uint8_t *mem;                       // full disk image
	long size;
	unsigned cyls, heads, spt, ssize;
	unsigned spare;                    // sectors hidden from CROOK (cylinder 0)

	unsigned area_start;              // logical sector of the CROOK area we overlay into
	bool sync;                         // two-way sync active (dir usable)
	struct tracked *files;             // tracked host files
	unsigned nfiles, cap;
	uint64_t *base_keys;               // FILDIC entry keys present in the base image
	unsigned nbase;
	bool guest_dirty;                 // CROOK wrote since the last guest->host sync
	long last_write, last_poll;

	dirwatch_t *watch;                 // NULL -> fall back to timed polling
	atomic_bool host_dirty;            // set by the watcher thread on any host change
	atomic_llong host_evt;            // time of the last host change (quiesce gate)

	fsmeta_t *meta;                    // .crookfs.ini - CROOK attrs a host file can't carry
};

// R40: 3 chars/word. index: 0=space 1-26=A-Z 27-36=0-9 37=_ 38=% 39=#
static const char R40_ALPHABET[40] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_%#";

// ---------------------------------------------------------------------------
// misc helpers
// ---------------------------------------------------------------------------

static uint32_t crc32(const uint8_t *p, size_t n)
{
	uint32_t c = 0xffffffffu;
	for (size_t i = 0 ; i < n ; i++) {
		c ^= p[i];
		for (int k = 0 ; k < 8 ; k++)
			c = (c >> 1) ^ (0xedb88320u & (uint32_t)(-(int32_t)(c & 1)));
	}
	return ~c;
}

static long now_secs(void) { return (long) time(NULL); }

// ---------------------------------------------------------------------------
// low-level RAM image access (16-bit big-endian words, CROOK-logical sectors)
// ---------------------------------------------------------------------------

static uint8_t * lsec_ptr(winchester_dir_t *wd, unsigned lsec)
{
	return wd->mem + (long)(lsec + wd->spare) * wd->ssize;
}

static uint16_t rdw(const uint8_t *p, unsigned word)
{
	p += word * 2;
	return (uint16_t)((p[0] << 8) | p[1]);
}

static void wrw(uint8_t *p, unsigned word, uint16_t v)
{
	p += word * 2;
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)(v & 0xff);
}

// ---------------------------------------------------------------------------
// a CROOK-5 disk area (relative to the physical image)
// ---------------------------------------------------------------------------

struct c5_area {
	winchester_dir_t *wd;
	unsigned start;                    // area's logical start sector
	uint16_t A0, A1, A2, A3, AK;       // LABEL pointers (area-relative)
	unsigned fildic_sectors;
};

static uint8_t * area_sec(struct c5_area *a, unsigned area_rel_sec)
{
	return lsec_ptr(a->wd, a->start + area_rel_sec);
}

static bool area_open(struct c5_area *a, winchester_dir_t *wd, unsigned start)
{
	a->wd = wd;
	a->start = start;
	const uint8_t *label = lsec_ptr(wd, start);   // A0==0 => DICDIC start doubles as label
	a->A0 = rdw(label, 1);
	a->A1 = rdw(label, 2);
	a->A2 = rdw(label, 3);
	a->A3 = rdw(label, 4);
	a->AK = rdw(label, 5);
	if (!(a->A1 > a->A0 && a->A2 > a->A1 && a->A3 >= a->A2 && a->AK > a->A3)) {
		return false;
	}
	a->fildic_sectors = a->A2 - a->A1;
	return true;
}

// ---------------------------------------------------------------------------
// MAPA - allocation bitmap (1 bit/sector, MSB first, bit set = allocated)
// ---------------------------------------------------------------------------

static uint8_t * map_byte(struct c5_area *a, unsigned sec)
{
	unsigned byte = sec >> 3;
	return area_sec(a, a->A2 + byte / a->wd->ssize) + (byte % a->wd->ssize);
}

static bool map_get(struct c5_area *a, unsigned sec)
{
	return (*map_byte(a, sec) >> (7 - (sec & 7))) & 1;
}
static void map_set(struct c5_area *a, unsigned sec)
{
	*map_byte(a, sec) |= (uint8_t)(0x80 >> (sec & 7));
}
static void map_clear(struct c5_area *a, unsigned sec)
{
	*map_byte(a, sec) &= (uint8_t) ~(0x80 >> (sec & 7));
}

// contiguous free run of `len` sectors, scanned from the top of the data area
// down - overlaid files land in high sectors, away from CROOK's own files.
static long map_find_run(struct c5_area *a, unsigned len)
{
	if (len == 0 || a->AK <= a->A3 + len) return -1;
	unsigned run = 0;
	for (unsigned sec = a->AK - 1 ; sec > a->A3 ; sec--) {
		if (map_get(a, sec)) {
			run = 0;
		} else if (++run == len) {
			return (long) sec;
		}
	}
	return -1;
}

// ---------------------------------------------------------------------------
// FILDIC
// ---------------------------------------------------------------------------

// CROOK's FILDIC hash (kernel routine at 0x19bf). Add the two R40 name words,
// fold in the byte-swapped sum, mask with the largest 2^k-1 below the FILDIC
// sector count. The label goes in FILDIC sector A1 + hash.
static unsigned fildic_hash(struct c5_area *a, uint16_t w0, uint16_t w1)
{
	unsigned sum = (unsigned)(w0 + w1) & 0xffff;
	unsigned folded = (sum + (((sum << 8) | (sum >> 8)) & 0xffff)) & 0xffff;
	unsigned mask = 1;
	while (mask < a->fildic_sectors) mask = (mask << 1) | 1;
	mask >>= 1;
	return folded & mask;
}

static uint16_t fildic_next_file_id(struct c5_area *a)
{
	uint8_t *sec = area_sec(a, a->A1 + FILDIC_COUNTER_SECTOR);
	uint16_t cur = rdw(sec, FILDIC_COUNTER_WORD);
	wrw(sec, FILDIC_COUNTER_WORD, (uint16_t)(cur + 1));
	return cur;
}

// clone param2 / rights / word8 from an existing entry with the same extension
static void fildic_clone_template(struct c5_area *a, uint16_t ext_w,
                                  uint16_t *p2, uint16_t *w6)
{
	for (unsigned rel = 0 ; rel < a->fildic_sectors ; rel++) {
		uint8_t *sec = area_sec(a, a->A1 + rel);
		for (unsigned e = 0 ; e < FILDIC_SLOTS ; e++) {
			unsigned o = e * FILDIC_ENTRY_WORDS;
			uint16_t w0 = rdw(sec, o);
			if (w0 == 0 || w0 == 1 || rdw(sec, o + 2) == 0) continue;
			if (rdw(sec, o + 3) == ext_w) {
				*p2 = rdw(sec, o + 5);
				*w6 = rdw(sec, o + 6);
				return;
			}
		}
	}
}

// locate a live entry by name; returns a pointer to its 12 words, or NULL
static uint16_t * fildic_find(struct c5_area *a, uint16_t w0, uint16_t w1)
{
	for (unsigned rel = 0 ; rel < a->fildic_sectors ; rel++) {
		uint8_t *sec = area_sec(a, a->A1 + rel);
		for (unsigned e = 0 ; e < FILDIC_SLOTS ; e++) {
			unsigned o = e * FILDIC_ENTRY_WORDS;
			uint16_t e0 = rdw(sec, o);
			if (e0 == 1) break;
			if (e0 == w0 && rdw(sec, o + 1) == w1) {
				static uint16_t ent[FILDIC_ENTRY_WORDS];
				for (unsigned k = 0 ; k < FILDIC_ENTRY_WORDS ; k++) ent[k] = rdw(sec, o + k);
				return ent;
			}
		}
	}
	return NULL;
}

// tombstone an entry (word0 -> 0) and free its data run in MAPA
static void fildic_remove(struct c5_area *a, uint16_t w0, uint16_t w1)
{
	for (unsigned rel = 0 ; rel < a->fildic_sectors ; rel++) {
		uint8_t *sec = area_sec(a, a->A1 + rel);
		for (unsigned e = 0 ; e < FILDIC_SLOTS ; e++) {
			unsigned o = e * FILDIC_ENTRY_WORDS;
			uint16_t e0 = rdw(sec, o);
			if (e0 == 1) break;
			if (e0 == w0 && rdw(sec, o + 1) == w1) {
				unsigned start = rdw(sec, o + 9), nsec = rdw(sec, o + 11);
				for (unsigned i = 0 ; i < nsec ; i++) map_clear(a, start + i);
				wrw(sec, o, 0);                  // tombstone
				return;
			}
		}
	}
}

// insert a 12-word entry at its hash sector: reuse a tombstone (word0 == 0) in
// place, or consume the end-of-dictionary marker (word0 == 1) and write a fresh
// marker into the following slot. Spills forward if the hash sector is full.
static bool fildic_insert(struct c5_area *a, const uint16_t ent[FILDIC_ENTRY_WORDS])
{
	unsigned h = fildic_hash(a, ent[0], ent[1]);
	for (unsigned rel = h ; rel < a->fildic_sectors ; rel++) {
		uint8_t *sec = area_sec(a, a->A1 + rel);
		for (unsigned e = 0 ; e < FILDIC_SLOTS - 1 ; e++) {
			uint16_t w0 = rdw(sec, e * FILDIC_ENTRY_WORDS);
			if (w0 != 0 && w0 != 1) continue;
			for (unsigned k = 0 ; k < FILDIC_ENTRY_WORDS ; k++)
				wrw(sec, e * FILDIC_ENTRY_WORDS + k, ent[k]);
			if (w0 == 1) {                       // consumed the marker -> re-emit it
				unsigned m = (e + 1) * FILDIC_ENTRY_WORDS;
				wrw(sec, m, 1);
				for (unsigned k = 1 ; k < FILDIC_ENTRY_WORDS ; k++) wrw(sec, m + k, 0);
			}
			return true;
		}
	}
	return false;
}

// snapshot every FILDIC entry key present right now (used before overlay to tell
// base-image files from host/user files)
static void fildic_snapshot_base(winchester_dir_t *wd, struct c5_area *a)
{
	unsigned cap = 256;
	wd->base_keys = malloc(cap * sizeof(uint64_t));
	wd->nbase = 0;
	for (unsigned rel = 0 ; rel < a->fildic_sectors && wd->base_keys ; rel++) {
		uint8_t *sec = area_sec(a, a->A1 + rel);
		for (unsigned e = 0 ; e < FILDIC_SLOTS ; e++) {
			unsigned o = e * FILDIC_ENTRY_WORDS;
			uint16_t w0 = rdw(sec, o);
			if (w0 == 0 || w0 == 1) continue;
			if (wd->nbase == cap) {
				cap *= 2;
				uint64_t *n = realloc(wd->base_keys, cap * sizeof(uint64_t));
				if (!n) break;
				wd->base_keys = n;
			}
			wd->base_keys[wd->nbase++] =
				((uint64_t)w0 << 32) | ((uint64_t)rdw(sec, o + 1) << 16) | rdw(sec, o + 3);
		}
	}
}

static bool is_base_file(winchester_dir_t *wd, uint16_t w0, uint16_t w1, uint16_t ew)
{
	uint64_t key = ((uint64_t)w0 << 32) | ((uint64_t)w1 << 16) | ew;
	for (unsigned i = 0 ; i < wd->nbase ; i++)
		if (wd->base_keys[i] == key) return true;
	return false;
}

// ---------------------------------------------------------------------------
// name handling
// ---------------------------------------------------------------------------

static int r40_index(char c)
{
	c = (char)toupper((unsigned char)c);
	for (int i = 0 ; i < 40 ; i++) if (R40_ALPHABET[i] == c) return i;
	return 0;                                        // unrepresentable -> space
}

static void r40_pack(const char *s, unsigned n, uint16_t *out)
{
	for (unsigned w = 0 ; w * 3 < n ; w++) {
		uint16_t v = 0;
		for (unsigned i = 0 ; i < 3 ; i++) {
			unsigned idx = w * 3 + i;
			v = (uint16_t)(v * 40 + (idx < strlen(s) ? r40_index(s[idx]) : 0));
		}
		out[w] = v;
	}
}

static void r40_unpack(uint16_t w, char out[3])
{
	out[0] = R40_ALPHABET[(w / 1600) % 40];
	out[1] = R40_ALPHABET[(w / 40) % 40];
	out[2] = R40_ALPHABET[w % 40];
}

// R40 name/ext words -> trimmed uppercase strings, matching split_name()'s output
static void r40_words_to_str(uint16_t w0, uint16_t w1, uint16_t ew,
                             char name[MAX_NAME + 1], char ext[MAX_EXT + 1])
{
	char t[MAX_NAME];
	r40_unpack(w0, t); r40_unpack(w1, t + 3);
	memcpy(name, t, MAX_NAME); name[MAX_NAME] = 0;
	r40_unpack(ew, t); memcpy(ext, t, MAX_EXT); ext[MAX_EXT] = 0;
	for (int i = MAX_NAME - 1 ; i >= 0 && name[i] == ' ' ; i--) name[i] = 0;
	for (int i = MAX_EXT  - 1 ; i >= 0 && ext[i]  == ' ' ; i--) ext[i]  = 0;
}

static void split_name(const char *fn, char *name, char *ext)
{
	memset(name, 0, MAX_NAME + 1);
	memset(ext, 0, MAX_EXT + 1);
	const char *dot = strrchr(fn, '.');
	size_t nlen = dot ? (size_t)(dot - fn) : strlen(fn);
	if (nlen > MAX_NAME) nlen = MAX_NAME;
	for (size_t i = 0 ; i < nlen ; i++) name[i] = (char)toupper((unsigned char)fn[i]);
	if (dot) {
		size_t elen = strlen(dot + 1);
		if (elen > MAX_EXT) elen = MAX_EXT;
		for (size_t i = 0 ; i < elen ; i++) ext[i] = (char)toupper((unsigned char)dot[1 + i]);
	}
}

// build "NAME.EXT" (trimmed) for a host path
static void host_basename(const struct tracked *t, char *out, size_t cap)
{
	char n[MAX_NAME + 1], x[MAX_EXT + 1];
	snprintf(n, sizeof(n), "%s", t->name);
	snprintf(x, sizeof(x), "%s", t->ext);
	for (char *p = n + strlen(n) ; p > n && p[-1] == ' ' ; ) *--p = 0;
	for (char *p = x + strlen(x) ; p > x && p[-1] == ' ' ; ) *--p = 0;
	if (*x) snprintf(out, cap, "%s.%s", n, x);
	else    snprintf(out, cap, "%s", n);
}

// ---------------------------------------------------------------------------
// tracked-file table
// ---------------------------------------------------------------------------

static struct tracked * track_find(winchester_dir_t *wd, const char *name, const char *ext)
{
	for (unsigned i = 0 ; i < wd->nfiles ; i++)
		if (!strcmp(wd->files[i].name, name) && !strcmp(wd->files[i].ext, ext))
			return &wd->files[i];
	return NULL;
}

static struct tracked * track_add(winchester_dir_t *wd)
{
	if (wd->nfiles == wd->cap) {
		unsigned nc = wd->cap ? wd->cap * 2 : 16;
		struct tracked *n = realloc(wd->files, nc * sizeof(*n));
		if (!n) return NULL;
		wd->files = n;
		wd->cap = nc;
	}
	struct tracked *t = &wd->files[wd->nfiles++];
	memset(t, 0, sizeof(*t));
	return t;
}

static void track_del(winchester_dir_t *wd, struct tracked *t)
{
	unsigned i = (unsigned)(t - wd->files);
	wd->files[i] = wd->files[--wd->nfiles];
}

// ---------------------------------------------------------------------------
// overlay / re-overlay one host file
// ---------------------------------------------------------------------------

static uint32_t image_data_crc(struct c5_area *a, unsigned start, unsigned nsec)
{
	return crc32(area_sec(a, start), (size_t)nsec * a->wd->ssize);
}

// (re)write a host file into the RAM image; updates/creates the tracked record
// remember the CROOK-only attributes of one entry, keyed by host filename
static void meta_capture(winchester_dir_t *wd, const char *hostname, const uint16_t *ent)
{
	if (!wd->meta) return;
	fsmeta_set_u(wd->meta, hostname, "owner",  ent[2]);
	fsmeta_set_u(wd->meta, hostname, "param2", ent[5]);
	fsmeta_set_u(wd->meta, hostname, "rights", ent[6]);
	fsmeta_set_u(wd->meta, hostname, "word7",  ent[7]);
}

// apply any sidecar overrides onto a freshly built entry
static void meta_apply(winchester_dir_t *wd, const char *hostname, uint16_t *ent)
{
	unsigned long v;
	if (fsmeta_get_u(wd->meta, hostname, "owner",  &v)) { ent[2] = (uint16_t) v; ent[7] = (uint16_t) v; }
	if (fsmeta_get_u(wd->meta, hostname, "param2", &v)) ent[5] = (uint16_t) v;
	if (fsmeta_get_u(wd->meta, hostname, "rights", &v)) ent[6] = (uint16_t) v;
	if (fsmeta_get_u(wd->meta, hostname, "word7",  &v)) ent[7] = (uint16_t) v;
}

static bool overlay_file(struct c5_area *a, const char *host_path, const char *fn,
                         const struct stat *st)
{
	winchester_dir_t *wd = a->wd;
	if (!S_ISREG(st->st_mode) || st->st_size > MAX_FILE_BYTES) return false;

	char name[MAX_NAME + 1], ext[MAX_EXT + 1];
	split_name(fn, name, ext);
	uint16_t nw[2] = {0, 0}, ew = 0;
	r40_pack(name, MAX_NAME, nw);
	r40_pack(ext, MAX_EXT, &ew);

	unsigned ssize = wd->ssize;
	unsigned nsec = (unsigned)((st->st_size + ssize - 1) / ssize);
	if (nsec == 0) nsec = 1;

	// drop any previous version of this file (ours or a stale entry)
	struct tracked *old = track_find(wd, name, ext);
	if (old || fildic_find(a, nw[0], nw[1]))
		fildic_remove(a, nw[0], nw[1]);

	long start = map_find_run(a, nsec);
	if (start < 0) {
		LOGWARN("winchester dir: no room for \"%s\" (%u sectors)", fn, nsec);
		if (old) track_del(wd, old);
		return false;
	}

	FILE *f = fopen(host_path, "rb");
	if (!f) { if (old) track_del(wd, old); return false; }
	uint8_t *dst = area_sec(a, (unsigned)start);
	memset(dst, 0, (size_t)nsec * ssize);
	size_t got = fread(dst, 1, (size_t)st->st_size, f);
	fclose(f);
	if (got != (size_t)st->st_size) {
		LOGWARN("winchester dir: short read on \"%s\"", fn);
		if (old) track_del(wd, old);
		return false;
	}
	for (unsigned i = 0 ; i < nsec ; i++) map_set(a, (unsigned)start + i);

	uint16_t p2 = 0, w6 = 0xa800;
	fildic_clone_template(a, ew, &p2, &w6);

	uint16_t ent[FILDIC_ENTRY_WORDS] = {0};
	ent[0] = nw[0]; ent[1] = nw[1];
	ent[2] = DICDIC_LIBRAR_CODE;
	ent[3] = ew;
	ent[4] = (uint16_t)((long)st->st_size - (long)nsec * ssize);   // param1
	ent[5] = p2;
	ent[6] = w6;
	ent[7] = DICDIC_LIBRAR_CODE;
	ent[8] = fildic_next_file_id(a);
	ent[9]  = (uint16_t)start;
	ent[10] = (uint16_t)(start + nsec);
	ent[11] = (uint16_t)nsec;

	meta_apply(wd, fn, ent);   // sidecar wins over the cloned template

	if (!fildic_insert(a, ent)) {
		for (unsigned i = 0 ; i < nsec ; i++) map_clear(a, (unsigned)start + i);
		LOGWARN("winchester dir: FILDIC full, dropped \"%s\"", fn);
		if (old) track_del(wd, old);
		return false;
	}

	struct tracked *t = old ? old : track_add(wd);
	if (!t) return false;
	snprintf(t->name, sizeof(t->name), "%s", name);
	snprintf(t->ext, sizeof(t->ext), "%s", ext);
	t->w0 = nw[0]; t->w1 = nw[1]; t->ew = ew;
	t->start = (unsigned)start; t->nsec = nsec;
	t->host_size = st->st_size;
	t->host_mtime = (long)st->st_mtime;
	t->crc = image_data_crc(a, (unsigned)start, nsec);
	meta_capture(wd, fn, ent);   // surface the attrs so they're stable + user-editable
	LOG(L_WNCH, "winchester dir: host->guest  %s.%s  %u sect @ %ld", name, ext, nsec, start);
	return true;
}

// ---------------------------------------------------------------------------
// host -> guest : rescan the directory, apply adds / changes / deletes
// ---------------------------------------------------------------------------

struct scan_ent { char raw[512]; char name[MAX_NAME + 1], ext[MAX_EXT + 1]; struct stat st; };

static void host_to_guest(winchester_dir_t *wd, struct c5_area *a)
{
	DIR *dp = opendir(wd->dir_name);
	if (!dp) return;

	// snapshot the directory first, so overlays growing the tracked list can't
	// perturb the delete-detection pass
	struct scan_ent *list = NULL;
	unsigned n = 0, cap = 0;
	struct dirent *de;
	while ((de = readdir(dp)) != NULL) {
		if (de->d_name[0] == '.') continue;
		char path[2048];
		snprintf(path, sizeof(path), "%s/%s", wd->dir_name, de->d_name);
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 16;
			struct scan_ent *nl = realloc(list, cap * sizeof(*nl));
			if (!nl) break;
			list = nl;
		}
		snprintf(list[n].raw, sizeof(list[n].raw), "%s", de->d_name);
		split_name(de->d_name, list[n].name, list[n].ext);
		list[n].st = st;
		n++;
	}
	closedir(dp);

	// tracked files no longer on the host -> remove from the image
	for (unsigned i = 0 ; i < wd->nfiles ; ) {
		struct tracked *t = &wd->files[i];
		bool on_host = false;
		for (unsigned j = 0 ; j < n ; j++)
			if (!strcmp(list[j].name, t->name) && !strcmp(list[j].ext, t->ext)) { on_host = true; break; }
		if (!on_host) {
			char gone[64];
			host_basename(t, gone, sizeof(gone));
			fsmeta_forget(wd->meta, gone);
			LOG(L_WNCH, "winchester dir: host->guest  removed %s.%s", t->name, t->ext);
			fildic_remove(a, t->w0, t->w1);
			track_del(wd, t);
			continue;
		}
		i++;
	}

	// new / changed host files -> (re)overlay
	for (unsigned j = 0 ; j < n ; j++) {
		struct tracked *t = track_find(wd, list[j].name, list[j].ext);
		if (!t || t->host_mtime != (long)list[j].st.st_mtime || t->host_size != list[j].st.st_size) {
			char path[2048];
			snprintf(path, sizeof(path), "%s/%s", wd->dir_name, list[j].raw);
			overlay_file(a, path, list[j].raw, &list[j].st);
		}
	}
	free(list);
	fsmeta_save(wd->meta);
}

// ---------------------------------------------------------------------------
// guest -> host : after CROOK settled, write back changed / new / deleted files
// ---------------------------------------------------------------------------

static void write_host_file(winchester_dir_t *wd, struct c5_area *a, struct tracked *t,
                            const uint16_t *ent)
{
	char base[64], path[2048];
	host_basename(t, base, sizeof(base));
	snprintf(path, sizeof(path), "%s/%s", wd->dir_name, base);

	unsigned start = ent[9], nsec = ent[11];
	long len = (long)nsec * wd->ssize;
	int16_t p1 = (int16_t) ent[4];             // negative: bytes short of a full sector
	if (p1 < 0 && -p1 < len) len += p1;        // trim padding of the last sector

	FILE *f = fopen(path, "wb");
	if (!f) { LOGWARN("winchester dir: cannot write back \"%s\"", path); return; }
	fwrite(area_sec(a, start), 1, (size_t)len, f);
	fclose(f);

	meta_capture(wd, base, ent);   // keep CROOK's attrs across the round trip

	struct stat st;
	t->start = start; t->nsec = nsec;
	t->crc = image_data_crc(a, start, nsec);
	t->host_size = len;
	if (stat(path, &st) == 0) t->host_mtime = (long)st.st_mtime;
	LOG(L_WNCH, "winchester dir: guest->host  %s (%ld bytes)", base, len);
}

static void guest_to_host(winchester_dir_t *wd, struct c5_area *a)
{
	// existing tracked files: pushed back if CROOK changed or deleted them
	for (unsigned i = 0 ; i < wd->nfiles ; ) {
		struct tracked *t = &wd->files[i];
		uint16_t *ent = fildic_find(a, t->w0, t->w1);
		if (!ent) {
			char base[64], path[2048];
			host_basename(t, base, sizeof(base));
			snprintf(path, sizeof(path), "%s/%s", wd->dir_name, base);
			remove(path);
			fsmeta_forget(wd->meta, base);
			LOG(L_WNCH, "winchester dir: guest->host  deleted %s", base);
			track_del(wd, t);
			continue;
		}
		if (image_data_crc(a, ent[9], ent[11]) != t->crc)
			write_host_file(wd, a, t, ent);
		i++;
	}

	// files CROOK created under LIBRAR that aren't from the base image
	for (unsigned rel = 0 ; rel < a->fildic_sectors ; rel++) {
		uint8_t *sec = area_sec(a, a->A1 + rel);
		for (unsigned e = 0 ; e < FILDIC_SLOTS ; e++) {
			unsigned o = e * FILDIC_ENTRY_WORDS;
			uint16_t w0 = rdw(sec, o);
			if (w0 == 1) break;
			if (w0 == 0) continue;
			if (rdw(sec, o + 2) != DICDIC_LIBRAR_CODE) continue;
			uint16_t w1 = rdw(sec, o + 1), ew = rdw(sec, o + 3);
			if (is_base_file(wd, w0, w1, ew)) continue;

			char name[MAX_NAME + 1], ext[MAX_EXT + 1];
			r40_words_to_str(w0, w1, ew, name, ext);
			if (track_find(wd, name, ext)) continue;

			struct tracked *t = track_add(wd);
			if (!t) return;
			snprintf(t->name, sizeof(t->name), "%s", name);
			snprintf(t->ext, sizeof(t->ext), "%s", ext);
			t->w0 = w0; t->w1 = w1; t->ew = ew;
			t->crc = ~image_data_crc(a, rdw(sec, o + 9), rdw(sec, o + 11)); // force write
			uint16_t ent[FILDIC_ENTRY_WORDS];
			for (unsigned k = 0 ; k < FILDIC_ENTRY_WORDS ; k++) ent[k] = rdw(sec, o + k);
			write_host_file(wd, a, t, ent);
		}
	}

	fsmeta_save(wd->meta);
}

// ---------------------------------------------------------------------------
// sync scheduling - called from the sector I/O path
// ---------------------------------------------------------------------------

// fired on the dirwatch thread - must stay short and lock-free
static void on_host_change(void *user)
{
	winchester_dir_t *wd = user;
	atomic_store(&wd->host_evt, (long long) now_secs());
	atomic_store(&wd->host_dirty, true);
}

static void maybe_sync(winchester_dir_t *wd)
{
	if (!wd->sync) return;
	long t = now_secs();

	if (wd->guest_dirty && (t - wd->last_write) >= SYNC_QUIESCE_SECS) {
		struct c5_area a;
		if (area_open(&a, wd, wd->area_start)) guest_to_host(wd, &a);
		wd->guest_dirty = false;
		wd->last_poll = t;
		return;
	}
	if (wd->guest_dirty) return;

	if (wd->watch) {
		if (atomic_load(&wd->host_dirty)
		    && (t - (long) atomic_load(&wd->host_evt)) >= SYNC_QUIESCE_SECS) {
			atomic_store(&wd->host_dirty, false);
			struct c5_area a;
			if (area_open(&a, wd, wd->area_start)) host_to_guest(wd, &a);
			wd->last_poll = t;
		}
	} else if ((t - wd->last_poll) >= POLL_INTERVAL_SECS) {
		struct c5_area a;
		if (area_open(&a, wd, wd->area_start)) host_to_guest(wd, &a);
		wd->last_poll = t;
	}
}

// ---------------------------------------------------------------------------
// initial overlay
// ---------------------------------------------------------------------------

static bool librar_present(struct c5_area *a)
{
	uint8_t *d0 = area_sec(a, a->A0);
	char nm[6];
	r40_unpack(rdw(d0, DICDIC_LIBRAR_WOFF), nm);
	r40_unpack(rdw(d0, DICDIC_LIBRAR_WOFF + 1), nm + 3);
	return strncmp(nm, "LIBRAR", 6) == 0;
}

// pick which CROOK area to overlay the host directory into: the first *data*
// area (A0 == 0, no LABEL/SYSTEM) that has a LIBRAR directory, so on a bootable
// base image the system area is left alone and the files land on its data disk.
// CROOK areas sit on 4912-sector quantum boundaries ("kwant podziału"). If no
// such area is found (e.g. a synthesized disk, or a single-area image), overlay
// into the area at logical 0.
#define WINCH_QUANT 4912

static unsigned pick_overlay_area(winchester_dir_t *wd)
{
	unsigned usable = wd->cyls * wd->heads * wd->spt - wd->spare;
	for (unsigned start = 0 ; start + WINCH_QUANT <= usable ; start += WINCH_QUANT) {
		struct c5_area a;
		if (!area_open(&a, wd, start)) continue;
		if (a.A0 != 0) continue;                 // system area - leave it alone
		if (!librar_present(&a)) continue;
		if (start != 0)
			LOG(L_WNCH, "winchester dir: overlaying into data area at logical sector %u", start);
		return start;
	}
	return 0;
}

static void overlay_dir(winchester_dir_t *wd)
{
	struct c5_area a;
	if (!area_open(&a, wd, wd->area_start) || !librar_present(&a)) {
		LOGWARN("winchester dir: base image has no usable LIBRAR - directory not merged.");
		return;
	}

	fildic_snapshot_base(wd, &a);

	DIR *dp = opendir(wd->dir_name);
	if (!dp) {
		LOGWARN("winchester dir: cannot open \"%s\" - serving the base image as-is.", wd->dir_name);
		return;
	}
	unsigned added = 0;
	struct dirent *de;
	while ((de = readdir(dp)) != NULL) {
		if (de->d_name[0] == '.') continue;
		char path[2048];
		snprintf(path, sizeof(path), "%s/%s", wd->dir_name, de->d_name);
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
		if (overlay_file(&a, path, de->d_name, &st)) added++;
	}
	closedir(dp);

	wd->sync = true;
	wd->last_poll = now_secs();
	fsmeta_save(wd->meta);
	LOG(L_WNCH, "winchester dir: merged %u file(s) from \"%s\" into LIBRAR (two-way sync on)",
		added, wd->dir_name);
}

// ---------------------------------------------------------------------------
// data-only mode: no base image - synthesize a fresh, empty CROOK-5 data area
// (A0 = 0, no LABEL/SYSTEM) at logical 0. The layout matches what BOSS's `CFA`
// command produces for a plain data disk, and CROOK-5 8/15 auto-attaches it with
// no alarm. The exact constants are taken from area "D" of the reference image
// crook5-p8f-1.1.1.img:
//   A1=9  (9 DICDIC sectors), A2=109 (100 FILDIC sectors), A3=117 (8 MAPA sectors),
//   AK=29472 (6 quanta of 4912 sectors).
//   NDIREC region = A3 .. A3+117, zero-filled but marked allocated in MAPA.
//   Two users: LIBRAR (root) and BOSS.
//   FILDIC holds the 5 mandatory BOSS system pseudo-files (MAP/DICDIC/GLOBAL/
//   FILDIC/NDIREC) at their hash sectors, plus the per-sector hash-overflow
//   link words that CROOK validates on attach (a 3-level 64/32/4 cascade).
// ---------------------------------------------------------------------------

#define SYNTH_A1      9
#define SYNTH_A2      109      // 100 FILDIC sectors
#define SYNTH_A3      117      // 8 MAPA sectors
#define SYNTH_AK      29472    // 6 * 4912-sector quanta
#define SYNTH_NDIREC  117      // NDIREC region length (sectors), right after MAPA
#define SYNTH_BOSS_CODE   (12 * 4)   // BOSS is the 2nd DICDIC entry (word 12) -> code 48

// write a 12-word DICDIC entry at word offset `woff`, split 4/4/4 across the
// first three DICDIC sectors at the same offset (CROOK layout, opcr57 IV.7.1)
static void synth_dicdic_entry(winchester_dir_t *wd, unsigned woff, const uint16_t e[12])
{
	for (unsigned part = 0 ; part < 3 ; part++) {
		uint8_t *sec = lsec_ptr(wd, part);
		for (unsigned k = 0 ; k < 4 ; k++) wrw(sec, woff + k, e[part * 4 + k]);
	}
}

// one mandatory BOSS system pseudo-file entry in its FILDIC hash sector
static void synth_fildic_sys(winchester_dir_t *wd, unsigned rel, const char *name,
                             uint16_t ext_w, uint16_t w6,
                             uint16_t start, uint16_t end, uint16_t len)
{
	uint8_t *sec = lsec_ptr(wd, SYNTH_A1 + rel);
	uint16_t nm[2] = {0, 0};
	r40_pack(name, MAX_NAME, nm);
	uint16_t e[FILDIC_ENTRY_WORDS] = {0};
	e[0] = nm[0]; e[1] = nm[1];
	e[2] = SYNTH_BOSS_CODE;          // owner: BOSS
	e[3] = ext_w;                    // "type" = area name (R40, 1 word)
	e[6] = w6;
	e[7] = 4;                        // creator flags word (BOSS's system files use 0x0004)
	e[9] = start; e[10] = end; e[11] = len;
	for (unsigned k = 0 ; k < FILDIC_ENTRY_WORDS ; k++) wrw(sec, 0 * FILDIC_ENTRY_WORDS + k, e[k]);
	wrw(sec, 1 * FILDIC_ENTRY_WORDS, 1);   // end-of-dictionary marker in slot 1
}

static bool synth_data_area(winchester_dir_t *wd)
{
	unsigned usable = wd->cyls * wd->heads * wd->spt - wd->spare;
	if (usable <= SYNTH_AK) return false;

	const uint16_t A1 = SYNTH_A1, A2 = SYNTH_A2, A3 = SYNTH_A3, AK = SYNTH_AK;
	const unsigned struct_end = A3 + SYNTH_NDIREC;   // sectors 0..struct_end-1 stay allocated

	memset(wd->mem, 0, wd->size);

	uint16_t ename[2] = {0, 0};
	r40_pack("DAT", 3, ename);                       // area name (1 R40 word, ename[0])

	// metryka: DICDIC words 0..7, mirrored in the first three DICDIC sectors
	uint16_t meta[8] = { ename[0], 0 /*A0*/, A1, A2, A3, AK, 0, 0 };
	for (unsigned s = 0 ; s < 3 ; s++) {
		uint8_t *sec = lsec_ptr(wd, s);
		for (unsigned w = 0 ; w < 8 ; w++) wrw(sec, w, meta[w]);
	}

	// users: LIBRAR (word 8, root) and BOSS (word 12, child of LIBRAR)
	uint16_t librar[12] = {0}, boss[12] = {0};
	r40_pack("LIBRAR", 6, librar);
	librar[3] = 1;            // one subdir (BOSS)
	librar[5] = 0x7fff;       // budget
	librar[7] = 0x7fff;       // rights
	r40_pack("BOSS", 6, boss);
	boss[2] = DICDIC_LIBRAR_CODE;   // parent = LIBRAR
	boss[5] = 0x7fff;
	boss[6] = DICDIC_LIBRAR_CODE;
	boss[7] = 0x7fff;
	synth_dicdic_entry(wd, DICDIC_LIBRAR_WOFF, librar);
	synth_dicdic_entry(wd, DICDIC_LIBRAR_WOFF + 4, boss);
	{ uint16_t end[12] = { 1, 0,0,0,0,0,0,0,0,0,0,0 };
	  synth_dicdic_entry(wd, DICDIC_LIBRAR_WOFF + 8, end); }   // end-of-dictionary

	// FILDIC: per-sector hash-cascade link words (100 sectors = 64+32+4 levels).
	// [252]=end-of-FILDIC flag, [253]=overflow link, [254]=level mask, [255]=index.
	for (unsigned rel = 0 ; rel < (unsigned)(A2 - A1) ; rel++) {
		uint8_t *sec = lsec_ptr(wd, A1 + rel);
		uint16_t link, mask, idx;
		if (rel < 64)       { link = (uint16_t)(A1 + 64 + (rel % 32));      mask = 63; idx = (uint16_t)rel; }
		else if (rel < 96)  { link = (uint16_t)(A1 + 96 + ((rel - 64) % 4)); mask = 31; idx = (uint16_t)(rel - 64); }
		else                { link = 0;                                     mask = 3;  idx = (uint16_t)(rel - 96); }
		wrw(sec, 0, 1);                       // slot 0 = end marker (overwritten for the 5 sys sectors)
		wrw(sec, 252, (rel == (unsigned)(A2 - A1) - 1) ? 0xffff : 0);
		wrw(sec, 253, link);
		wrw(sec, 254, mask);
		wrw(sec, 255, idx);
	}

	// the 5 mandatory BOSS system pseudo-files, at their fixed hash sectors
	synth_fildic_sys(wd,  9, "MAP",    ename[0], 0x8000, A2, A3,              (uint16_t)(A3 - A2));
	synth_fildic_sys(wd, 10, "DICDIC", ename[0], 0x8000, 0,  A1,             A1);
	synth_fildic_sys(wd, 29, "GLOBAL", ename[0], 0x8000, 0,  AK,             AK);
	synth_fildic_sys(wd, 32, "FILDIC", ename[0], 0x8000, A1, A2,             (uint16_t)(A2 - A1));
	synth_fildic_sys(wd, 61, "NDIREC", ename[0], 0xc000, A3, (uint16_t)(A3 + SYNTH_NDIREC), SYNTH_NDIREC);

	// MAPA: sectors 0 .. A3+NDIREC-1 allocated, rest free
	struct c5_area a;
	if (!area_open(&a, wd, 0)) return false;
	for (unsigned s = 0 ; s < struct_end ; s++) map_set(&a, s);

	LOG(L_WNCH, "winchester dir: synthesized empty CROOK-5 data area (A1=%u A2=%u A3=%u AK=%u, %u sectors free)",
		A1, A2, A3, AK, AK - struct_end);
	return true;
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
	wd->dir_name = dir_name ? strdup(dir_name) : NULL;

	wd->mem = malloc(wd->size);
	if (!wd->mem) {
		LOGERR("Failed to allocate %li bytes for the directory-backed winchester image.", wd->size);
		winchester_dir_destroy(wd);
		return NULL;
	}

	if (image_name && *image_name) {
		// overlay mode: inject the host directory into a real CROOK-5 image
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
		// data-only mode: no base image, synthesize an empty CROOK-5 data disk
		if (!synth_data_area(wd)) {
			LOGERR("Failed to synthesize the data-only winchester area.");
			winchester_dir_destroy(wd);
			return NULL;
		}
	}

	if (wd->dir_name && *wd->dir_name) {
		wd->area_start = pick_overlay_area(wd);
		wd->meta = fsmeta_load(wd->dir_name, FSMETA_SIDECAR);
		overlay_dir(wd);
	}

	const char *dump = getenv("WINCH_DIR_DUMP");
	if (dump && *dump) {
		FILE *d = fopen(dump, "wb");
		if (d) { fwrite(wd->mem, 1, wd->size, d); fclose(d); }
	}

	if (wd->sync) {
		wd->watch = dirwatch_create(wd->dir_name, on_host_change, wd);
		LOG(L_WNCH, "winchester dir: host watch %s", wd->watch ? "active" : "unavailable, polling");
	}

	return wd;
}

// -----------------------------------------------------------------------
void winchester_dir_destroy(winchester_dir_t *wd)
{
	if (!wd) return;
	if (wd->watch) dirwatch_destroy(wd->watch);   // no more host events past here
	if (wd->sync) {                    // final flush of anything CROOK changed
		struct c5_area a;
		if (area_open(&a, wd, wd->area_start)) guest_to_host(wd, &a);
	}
	fsmeta_save(wd->meta);
	fsmeta_free(wd->meta);
	free(wd->base_keys);
	free(wd->files);
	free(wd->mem);
	free(wd->dir_name);
	free(wd);
}

// -----------------------------------------------------------------------
int winchester_dir_sector_rd(winchester_dir_t *wd, uint8_t *buf, unsigned c, unsigned h, unsigned s)
{
	long off = wd_offset(wd, c, h, s);
	if (off < 0) return DEV_STATUS_SEEKERR;
	maybe_sync(wd);
	memcpy(buf, wd->mem + off, wd->ssize);
	return DEV_STATUS_OK;
}

// -----------------------------------------------------------------------
int winchester_dir_sector_wr(winchester_dir_t *wd, uint8_t *buf, unsigned c, unsigned h, unsigned s)
{
	long off = wd_offset(wd, c, h, s);
	if (off < 0) return DEV_STATUS_SEEKERR;
	memcpy(wd->mem + off, buf, wd->ssize);
	if (wd->sync) { wd->guest_dirty = true; wd->last_write = now_secs(); }
	return DEV_STATUS_OK;
}

// vim: tabstop=4 shiftwidth=4 autoindent
