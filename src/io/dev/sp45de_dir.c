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
#include "io/dev/dirwatch.h"
#include "io/dev/sp45de_dir.h"

// IBM 3740 basic data exchange, single-sided. Track 0 index layout:
//   S1-S4  reserved / DDR         S5  ERMAP        S7  VOL1
//   S8..S26  HDR1 records (one per file, up to 19)
// Files: contiguous extents on tracks 1..T-1, addressed "TTSSS" (track, sector).
// Data records are stored raw, one host byte stream padded to the sector; the
// HDR1 record length is set to the sector size.
//
// This models the SP45DE's physical 3740 format only. It is not a CROOK-5
// filesystem and CROOK-5 will not attach it as a disk area - see sp45de_dir.h.

#define IDX_ERMAP   5
#define IDX_VOL1    7
#define IDX_HDR1_LO 8
#define IDX_HDR1_HI 26
#define MAX_FILES   (IDX_HDR1_HI - IDX_HDR1_LO + 1)     // 19
#define NAME_LEN    8
#define SYNC_QUIESCE_SECS  1
#define POLL_INTERVAL_SECS 2

struct tf {
	char name[NAME_LEN + 1];       // host base name, upper, <= 8, no dot part kept
	unsigned boe_t, boe_s;         // begin of extent
	unsigned nsec;
	long host_size, host_mtime;
	uint32_t crc;
	unsigned hdr_slot;             // index-track sector holding this file's HDR1
};

struct sp45de_dir {
	char *dir_name;
	uint8_t *mem;
	long size;
	unsigned tracks, spt, blk;
	unsigned data_last;            // last track usable for file data (tracks past it are reserved)

	bool sync;
	struct tf files[MAX_FILES];
	unsigned nfiles;
	bool guest_dirty;
	long last_write, last_poll;

	dirwatch_t *watch;                 // NULL -> fall back to timed polling
	atomic_bool host_dirty;            // set by the watcher thread on any host change
	atomic_llong host_evt;            // time of the last host change (quiesce gate)
};

// --- EBCDIC (cp037) <-> ASCII, only the printable range we need ---------------
static uint8_t a2e(char c)
{
	static const char *a = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,/-()";
	static const uint8_t e[] = {
		0x40,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xD1,0xD2,0xD3,0xD4,
		0xD5,0xD6,0xD7,0xD8,0xD9,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,
		0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,
		0x4B,0x6B,0x61,0x60,0x4D,0x5D };
	c = (char)toupper((unsigned char)c);
	const char *p = strchr(a, c);
	return p ? e[p - a] : 0x40;
}

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

// --- raw sector access ------------------------------------------------------
static uint8_t * blk(sp45de_dir_t *sd, unsigned track, unsigned sector)
{
	long off = (long)(track * sd->spt + (sector - 1)) * sd->blk;
	if (track >= sd->tracks || sector < 1 || sector > sd->spt || off + sd->blk > sd->size)
		return NULL;
	return sd->mem + off;
}

// data-area linear sector index <-> (track, sector), tracks 1..T-1
static void lin_to_ts(sp45de_dir_t *sd, unsigned lin, unsigned *t, unsigned *s)
{
	*t = 1 + lin / sd->spt;
	*s = 1 + lin % sd->spt;
}
static unsigned data_sectors(sp45de_dir_t *sd) { return sd->data_last * sd->spt; }

// --- index track (track 0) ------------------------------------------------
static void ebcdic_field(uint8_t *dst, const char *txt, unsigned len)
{
	for (unsigned i = 0 ; i < len ; i++)
		dst[i] = a2e(txt[i] ? txt[i] : ' ');
}

static void write_ts5(uint8_t *dst, unsigned t, unsigned s)
{
	char b[6];
	snprintf(b, sizeof(b), "%02u%03u", t, s);
	ebcdic_field(dst, b, 5);
}

// build a fresh, empty IBM 3740 index track
static void index_init(sp45de_dir_t *sd)
{
	memset(sd->mem, 0, (size_t)sd->spt * sd->blk);          // whole track 0
	uint8_t *b;
	if ((b = blk(sd, 0, IDX_ERMAP))) { memset(b, a2e(' '), sd->blk); ebcdic_field(b, "ERMAP", 5); }
	if ((b = blk(sd, 0, IDX_VOL1)))  { memset(b, a2e(' '), sd->blk); ebcdic_field(b, "VOL1", 4);
	                                   b[79] = a2e('W'); }   // byte 79 = access indicator ('W' = owner-only, per sample
	for (unsigned s = IDX_HDR1_LO ; s <= IDX_HDR1_HI ; s++)
		if ((b = blk(sd, 0, s))) memset(b, a2e(' '), sd->blk);   // empty HDR1 slots = blanks
}

// write one HDR1 record.  offsets follow the IBM 3740 / ISO 6596 layout:
//   0-3 HDR1  5-12 name  24-27 reclen  29-33 BOE  35-39 EOE  74-78 EOD
static void hdr1_write(sp45de_dir_t *sd, unsigned slot, const struct tf *f)
{
	uint8_t *b = blk(sd, 0, slot);
	if (!b) return;
	memset(b, a2e(' '), sd->blk);
	ebcdic_field(b + 0, "HDR1", 4);
	ebcdic_field(b + 5, f->name, NAME_LEN);
	{ char rl[5]; snprintf(rl, sizeof(rl), "%04u", sd->blk); ebcdic_field(b + 24, rl, 4); }
	write_ts5(b + 29, f->boe_t, f->boe_s);
	// EOE = last sector of the extent
	unsigned lin0 = (f->boe_t - 1) * sd->spt + (f->boe_s - 1);
	unsigned et, es;
	lin_to_ts(sd, lin0 + f->nsec - 1, &et, &es);
	write_ts5(b + 35, et, es);
	// EOD = first free sector after the file (data written up to here)
	lin_to_ts(sd, lin0 + f->nsec, &et, &es);
	write_ts5(b + 74, et, es);
}

// --- host name -> 8-char upper base ----------------------------------------
static void host_name8(const char *fn, char out[NAME_LEN + 1])
{
	memset(out, 0, NAME_LEN + 1);
	const char *dot = strrchr(fn, '.');
	size_t n = dot ? (size_t)(dot - fn) : strlen(fn);
	if (n > NAME_LEN) n = NAME_LEN;
	for (size_t i = 0 ; i < n ; i++) {
		char c = (char)toupper((unsigned char)fn[i]);
		out[i] = (isalnum((unsigned char)c) || c == '-') ? c : '_';
	}
}

// --- tracking table -------------------------------------------------------
static struct tf * tf_find(sp45de_dir_t *sd, const char *name)
{
	for (unsigned i = 0 ; i < sd->nfiles ; i++)
		if (!strcmp(sd->files[i].name, name)) return &sd->files[i];
	return NULL;
}

// pack all tracked files back-to-back from track 1, rebuild the index track
static void rebuild(sp45de_dir_t *sd)
{
	index_init(sd);
	unsigned lin = 0;
	for (unsigned i = 0 ; i < sd->nfiles ; i++) {
		struct tf *f = &sd->files[i];
		lin_to_ts(sd, lin, &f->boe_t, &f->boe_s);
		f->hdr_slot = IDX_HDR1_LO + i;
		hdr1_write(sd, f->hdr_slot, f);
		lin += f->nsec;
	}
}

// (re)load one host file into the image, updating its tracked record
static bool overlay_one(sp45de_dir_t *sd, const char *path, const char *fn, const struct stat *st)
{
	if (!S_ISREG(st->st_mode)) return false;
	char name[NAME_LEN + 1];
	host_name8(fn, name);

	unsigned nsec = (unsigned)((st->st_size + sd->blk - 1) / sd->blk);
	if (nsec == 0) nsec = 1;

	struct tf *f = tf_find(sd, name);
	// total sectors used by everyone else
	unsigned used = 0;
	for (unsigned i = 0 ; i < sd->nfiles ; i++)
		if (&sd->files[i] != f) used += sd->files[i].nsec;
	if (used + nsec > data_sectors(sd)) {
		LOGWARN("sp45de dir: no room for \"%s\" (%u sectors)", fn, nsec);
		return false;
	}
	if (!f) {
		if (sd->nfiles >= MAX_FILES) {
			LOGWARN("sp45de dir: index full (max %d files), dropped \"%s\"", MAX_FILES, fn);
			return false;
		}
		f = &sd->files[sd->nfiles++];
		memset(f, 0, sizeof(*f));
		snprintf(f->name, sizeof(f->name), "%s", name);
	}
	f->nsec = nsec;
	f->host_size = st->st_size;
	f->host_mtime = (long)st->st_mtime;

	rebuild(sd);   // assigns f->boe_t/s

	FILE *fp = fopen(path, "rb");
	if (!fp) return false;
	unsigned lin = (f->boe_t - 1) * sd->spt + (f->boe_s - 1);
	long rem = st->st_size;
	for (unsigned i = 0 ; i < nsec ; i++) {
		unsigned t, s;
		lin_to_ts(sd, lin + i, &t, &s);
		uint8_t *b = blk(sd, t, s);
		if (!b) break;
		memset(b, 0, sd->blk);
		long want = rem < (long)sd->blk ? rem : (long)sd->blk;
		if (want > 0) { if (fread(b, 1, (size_t)want, fp) != (size_t)want) break; rem -= want; }
	}
	fclose(fp);

	f->crc = crc32(blk(sd, f->boe_t, f->boe_s), (size_t)nsec * sd->blk);
	LOG(L_FLOP, "sp45de dir: host->guest  %-8s  %u sect @ T%uS%u", name, nsec, f->boe_t, f->boe_s);
	return true;
}

// --- host <-> guest sync -------------------------------------------------
struct scan { char raw[512], name[NAME_LEN + 1]; struct stat st; };

static void host_to_guest(sp45de_dir_t *sd)
{
	DIR *dp = opendir(sd->dir_name);
	if (!dp) return;
	struct scan list[MAX_FILES * 2];
	unsigned n = 0;
	struct dirent *de;
	while ((de = readdir(dp)) && n < MAX_FILES * 2) {
		if (de->d_name[0] == '.') continue;
		char path[2048];
		snprintf(path, sizeof(path), "%s/%s", sd->dir_name, de->d_name);
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
		snprintf(list[n].raw, sizeof(list[n].raw), "%s", de->d_name);
		host_name8(de->d_name, list[n].name);
		list[n].st = st;
		n++;
	}
	closedir(dp);

	// removed on host -> drop
	for (unsigned i = 0 ; i < sd->nfiles ; ) {
		bool on = false;
		for (unsigned j = 0 ; j < n ; j++)
			if (!strcmp(list[j].name, sd->files[i].name)) { on = true; break; }
		if (!on) {
			LOG(L_FLOP, "sp45de dir: host->guest  removed %s", sd->files[i].name);
			sd->files[i] = sd->files[--sd->nfiles];
			rebuild(sd);
			continue;
		}
		i++;
	}
	// new / changed -> (re)overlay
	for (unsigned j = 0 ; j < n ; j++) {
		struct tf *f = tf_find(sd, list[j].name);
		if (!f || f->host_mtime != (long)list[j].st.st_mtime || f->host_size != list[j].st.st_size) {
			char path[2048];
			snprintf(path, sizeof(path), "%s/%s", sd->dir_name, list[j].raw);
			overlay_one(sd, path, list[j].raw, &list[j].st);
		}
	}
}

static void guest_to_host(sp45de_dir_t *sd)
{
	// re-parse the index track: CROOK/whatever may have added or changed files
	for (unsigned slot = IDX_HDR1_LO ; slot <= IDX_HDR1_HI ; slot++) {
		uint8_t *b = blk(sd, 0, slot);
		if (!b || a2e('H') != b[0] || a2e('D') != b[1]) continue;   // not a HDR1
		// decode name
		char name[NAME_LEN + 1] = {0};
		static const char *tbl = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-/";
		for (int i = 0 ; i < NAME_LEN ; i++) {
			uint8_t e = b[5 + i];
			char c = ' ';
			for (const char *p = tbl ; *p ; p++) if (a2e(*p) == e) { c = *p; break; }
			name[i] = c;
		}
		for (int i = NAME_LEN - 1 ; i >= 0 && name[i] == ' ' ; i--) name[i] = 0;
		// the HDR1 name comes from guest-controlled sectors: never let it steer the
		// host path. Keep only [A-Z0-9-], everything else -> '_', reject dot names.
		for (char *p = name ; *p ; p++)
			if (!(isalnum((unsigned char)*p) || *p == '-')) *p = '_';
		{
			char *s = name;
			while (*s == '_') s++;
			if (!*s) continue;                 // empty / all-underscore
		}

		// EOD - BOE = sectors of actual data
		unsigned bt = 0, bs = 0, et = 0, es = 0;
		char tmp[6] = {0};
		for (int i = 0 ; i < 5 ; i++) { for (const char *p = "0123456789" ; *p ; p++) if (a2e(*p) == b[29 + i]) tmp[i] = *p; }
		sscanf(tmp, "%2u%3u", &bt, &bs);
		memset(tmp, 0, sizeof(tmp));
		for (int i = 0 ; i < 5 ; i++) { for (const char *p = "0123456789" ; *p ; p++) if (a2e(*p) == b[74 + i]) tmp[i] = *p; }
		sscanf(tmp, "%2u%3u", &et, &es);
		if (!bt) continue;

		unsigned lin0 = (bt - 1) * sd->spt + (bs - 1);
		unsigned lin1 = et ? (et - 1) * sd->spt + (es - 1) : lin0;
		unsigned nsec = lin1 > lin0 ? lin1 - lin0 : 0;

		struct tf *f = tf_find(sd, name);
		uint32_t cur = nsec ? crc32(blk(sd, bt, bs), (size_t)nsec * sd->blk) : 0;
		if (f && f->crc == cur) continue;

		char path[2048];
		snprintf(path, sizeof(path), "%s/%s", sd->dir_name, name);
		FILE *fp = fopen(path, "wb");
		if (fp) {
			if (nsec) fwrite(blk(sd, bt, bs), 1, (size_t)nsec * sd->blk, fp);
			fclose(fp);
			LOG(L_FLOP, "sp45de dir: guest->host  %s (%u sect)", name, nsec);
		}
		struct stat st;
		if (!f && sd->nfiles < MAX_FILES) { f = &sd->files[sd->nfiles++]; memset(f, 0, sizeof(*f)); snprintf(f->name, sizeof(f->name), "%s", name); }
		if (f) {
			f->nsec = nsec ? nsec : 1;
			f->crc = cur;
			if (stat(path, &st) == 0) { f->host_mtime = (long)st.st_mtime; f->host_size = st.st_size; }
		}
	}
}

// fired on the dirwatch thread - must stay short and lock-free
static void on_host_change(void *user)
{
	sp45de_dir_t *sd = user;
	atomic_store(&sd->host_evt, (long long) now_secs());
	atomic_store(&sd->host_dirty, true);
}

static void maybe_sync(sp45de_dir_t *sd)
{
	if (!sd->sync) return;
	long t = now_secs();

	if (sd->guest_dirty && (t - sd->last_write) >= SYNC_QUIESCE_SECS) {
		guest_to_host(sd);
		sd->guest_dirty = false;
		sd->last_poll = t;
		return;
	}
	if (sd->guest_dirty) return;

	if (sd->watch) {
		// event-driven: rescan once the host writes have settled
		if (atomic_load(&sd->host_dirty)
		    && (t - (long) atomic_load(&sd->host_evt)) >= SYNC_QUIESCE_SECS) {
			atomic_store(&sd->host_dirty, false);
			host_to_guest(sd);
			sd->last_poll = t;
		}
	} else if ((t - sd->last_poll) >= POLL_INTERVAL_SECS) {
		host_to_guest(sd);
		sd->last_poll = t;
	}
}

// --- public API ----------------------------------------------------------
sp45de_dir_t * sp45de_dir_create(const char *dir_name, unsigned tracks, unsigned data_last,
                                 unsigned spt, unsigned blk_size)
{
	if (!dir_name || !*dir_name) return NULL;
	sp45de_dir_t *sd = calloc(1, sizeof(*sd));
	if (!sd) return NULL;
	sd->tracks = tracks; sd->spt = spt; sd->blk = blk_size;
	sd->data_last = (data_last && data_last < tracks) ? data_last : tracks - 1;
	sd->size = (long)tracks * spt * blk_size;
	sd->dir_name = strdup(dir_name);
	sd->mem = calloc(1, sd->size);
	if (!sd->dir_name || !sd->mem) { sp45de_dir_destroy(sd); return NULL; }

	index_init(sd);

	DIR *dp = opendir(dir_name);
	if (!dp) {
		LOGWARN("sp45de dir: cannot open \"%s\" - empty diskette served.", dir_name);
	} else {
		struct dirent *de;
		unsigned added = 0;
		while ((de = readdir(dp)) != NULL) {
			if (de->d_name[0] == '.') continue;
			char path[2048];
			snprintf(path, sizeof(path), "%s/%s", dir_name, de->d_name);
			struct stat st;
			if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
			if (overlay_one(sd, path, de->d_name, &st)) added++;
		}
		closedir(dp);
		LOG(L_FLOP, "sp45de dir: %u file(s) from \"%s\" on the diskette (two-way sync on)", added, dir_name);
	}
	sd->sync = true;
	sd->last_poll = now_secs();
	sd->watch = dirwatch_create(dir_name, on_host_change, sd);
	LOG(L_FLOP, "sp45de dir: host watch %s", sd->watch ? "active" : "unavailable, polling");

	const char *dump = getenv("SP45DE_DIR_DUMP");
	if (dump) {
		FILE *fp = fopen(dump, "wb");
		if (fp) { fwrite(sd->mem, 1, (size_t)sd->size, fp); fclose(fp); }
	}
	return sd;
}

void sp45de_dir_destroy(sp45de_dir_t *sd)
{
	if (!sd) return;
	if (sd->watch) dirwatch_destroy(sd->watch);   // no more host events past here
	if (sd->sync) guest_to_host(sd);
	free(sd->mem);
	free(sd->dir_name);
	free(sd);
}

enum sp45de_result sp45de_dir_blk_rd(sp45de_dir_t *sd, unsigned track, unsigned sector, uint8_t *buf)
{
	uint8_t *b = blk(sd, track, sector);
	if (!b) return SP45DE_R_NO_SECTOR;
	maybe_sync(sd);
	memcpy(buf, b, sd->blk);
	return SP45DE_R_OK;
}

enum sp45de_result sp45de_dir_blk_wr(sp45de_dir_t *sd, unsigned track, unsigned sector, const uint8_t *buf)
{
	uint8_t *b = blk(sd, track, sector);
	if (!b) return SP45DE_R_NO_SECTOR;
	memcpy(b, buf, sd->blk);
	if (sd->sync) { sd->guest_dirty = true; sd->last_write = now_secs(); }
	return SP45DE_R_OK;
}

// vim: tabstop=4 shiftwidth=4 autoindent
