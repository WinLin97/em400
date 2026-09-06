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
#include <time.h>
#include <stdatomic.h>
#include <dirent.h>
#include <sys/stat.h>

#include "log.h"
#include "io/dev/dirwatch.h"
#include "io/dev/fsmeta.h"
#include "io/dev/crook5fs.h"

#define MAX_NAME C5FS_MAX_NAME
#define MAX_EXT  C5FS_MAX_EXT
#define FILDIC_ENTRY_WORDS C5FS_FILDIC_ENTRY_WORDS
#define FILDIC_SLOTS       C5FS_FILDIC_SLOTS
#define DICDIC_LIBRAR_WOFF C5FS_DICDIC_LIBRAR_WOFF
#define DICDIC_LIBRAR_CODE C5FS_DICDIC_LIBRAR_CODE
#define SSIZE C5FS_SSIZE

#define SYNC_QUIESCE_SECS  1
#define POLL_INTERVAL_SECS 2
#define MAX_FILE_BYTES     (20L * 1024 * 1024)

// one host file we injected / are tracking
struct tracked {
	char name[MAX_NAME + 1];
	char ext[MAX_EXT + 1];
	uint16_t w0, w1, ew;
	unsigned start, nsec;
	long host_size;
	long host_mtime;
	uint32_t crc;
	bool text_crlf;            // host file is text; LF<->CRLF translated on the way in/out
};

struct c5fs {
	uint8_t *mem;                       // caller-owned RAM image (512-byte logical sectors)
	long size;
	unsigned spare;                    // logical sectors hidden from CROOK addressing

	char *dir_name;
	unsigned area_start;
	bool sync;
	struct tracked *files;
	unsigned nfiles, cap;
	uint64_t *base_keys;
	unsigned nbase;
	bool guest_dirty;
	long last_write, last_poll;

	dirwatch_t *watch;
	atomic_bool host_dirty;
	atomic_llong host_evt;

	fsmeta_t *meta;
	int logc;
};

static const char R40_ALPHABET[40] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_%#";

// ---------------------------------------------------------------------------
// misc
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
// low-level image access
// ---------------------------------------------------------------------------

uint8_t * c5fs_lsec(c5fs_t *fs, unsigned lsec)
{
	return fs->mem + (long)(lsec + fs->spare) * SSIZE;
}

uint16_t c5fs_rdw(const uint8_t *p, unsigned word)
{
	p += word * 2;
	return (uint16_t)((p[0] << 8) | p[1]);
}

void c5fs_wrw(uint8_t *p, unsigned word, uint16_t v)
{
	p += word * 2;
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)(v & 0xff);
}

#define rdw c5fs_rdw
#define wrw c5fs_wrw

// ---------------------------------------------------------------------------
// area
// ---------------------------------------------------------------------------

uint8_t * c5fs_area_sec(struct c5_area *a, unsigned area_rel_sec)
{
	return c5fs_lsec(a->fs, a->start + area_rel_sec);
}
#define area_sec c5fs_area_sec

bool c5fs_area_open(struct c5_area *a, c5fs_t *fs, unsigned start)
{
	a->fs = fs;
	a->start = start;
	const uint8_t *label = c5fs_lsec(fs, start);
	a->A0 = rdw(label, 1);
	a->A1 = rdw(label, 2);
	a->A2 = rdw(label, 3);
	a->A3 = (uint16_t)(rdw(label, 4) & 0x7fff);     // T variant sets bit 15 as a flag
	a->AK = rdw(label, 5);
	if (!(a->A1 > a->A0 && a->A2 > a->A1 && a->A3 >= a->A2 && a->AK > a->A3)) {
		return false;
	}
	a->fildic_sectors = a->A2 - a->A1;
	return true;
}
#define area_open c5fs_area_open

// ---------------------------------------------------------------------------
// MAPA
// ---------------------------------------------------------------------------

static uint8_t * map_byte(struct c5_area *a, unsigned sec)
{
	unsigned byte = sec >> 3;
	return area_sec(a, a->A2 + byte / SSIZE) + (byte % SSIZE);
}
bool c5fs_map_get(struct c5_area *a, unsigned sec)
{
	return (*map_byte(a, sec) >> (7 - (sec & 7))) & 1;
}
void c5fs_map_set(struct c5_area *a, unsigned sec)
{
	*map_byte(a, sec) |= (uint8_t)(0x80 >> (sec & 7));
}
static void map_clear(struct c5_area *a, unsigned sec)
{
	*map_byte(a, sec) &= (uint8_t) ~(0x80 >> (sec & 7));
}
#define map_get c5fs_map_get
#define map_set c5fs_map_set

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

unsigned c5fs_fildic_hash(struct c5_area *a, uint16_t w0, uint16_t w1)
{
	unsigned sum = (unsigned)(w0 + w1) & 0xffff;
	unsigned folded = (sum + (((sum << 8) | (sum >> 8)) & 0xffff)) & 0xffff;
	unsigned mask = 1;
	while (mask < a->fildic_sectors) mask = (mask << 1) | 1;
	mask >>= 1;
	return folded & mask;
}
#define fildic_hash c5fs_fildic_hash


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
				wrw(sec, o, 0);
				return;
			}
		}
	}
}

bool c5fs_fildic_insert(struct c5_area *a, const uint16_t ent[FILDIC_ENTRY_WORDS])
{
	unsigned h = fildic_hash(a, ent[0], ent[1]);
	for (unsigned rel = h ; rel < a->fildic_sectors ; rel++) {
		uint8_t *sec = area_sec(a, a->A1 + rel);
		for (unsigned e = 0 ; e < FILDIC_SLOTS - 1 ; e++) {
			uint16_t w0 = rdw(sec, e * FILDIC_ENTRY_WORDS);
			if (w0 != 0 && w0 != 1) continue;
			for (unsigned k = 0 ; k < FILDIC_ENTRY_WORDS ; k++)
				wrw(sec, e * FILDIC_ENTRY_WORDS + k, ent[k]);
			if (w0 == 1) {
				unsigned m = (e + 1) * FILDIC_ENTRY_WORDS;
				wrw(sec, m, 1);
				for (unsigned k = 1 ; k < FILDIC_ENTRY_WORDS ; k++) wrw(sec, m + k, 0);
			}
			return true;
		}
	}
	return false;
}
#define fildic_insert c5fs_fildic_insert

static void fildic_snapshot_base(c5fs_t *fs, struct c5_area *a)
{
	unsigned cap = 256;
	fs->base_keys = malloc(cap * sizeof(uint64_t));
	fs->nbase = 0;
	for (unsigned rel = 0 ; rel < a->fildic_sectors && fs->base_keys ; rel++) {
		uint8_t *sec = area_sec(a, a->A1 + rel);
		for (unsigned e = 0 ; e < FILDIC_SLOTS ; e++) {
			unsigned o = e * FILDIC_ENTRY_WORDS;
			uint16_t w0 = rdw(sec, o);
			if (w0 == 0 || w0 == 1) continue;
			if (fs->nbase == cap) {
				cap *= 2;
				uint64_t *n = realloc(fs->base_keys, cap * sizeof(uint64_t));
				if (!n) break;
				fs->base_keys = n;
			}
			fs->base_keys[fs->nbase++] =
				((uint64_t)w0 << 32) | ((uint64_t)rdw(sec, o + 1) << 16) | rdw(sec, o + 3);
		}
	}
}

static bool is_base_file(c5fs_t *fs, uint16_t w0, uint16_t w1, uint16_t ew)
{
	uint64_t key = ((uint64_t)w0 << 32) | ((uint64_t)w1 << 16) | ew;
	for (unsigned i = 0 ; i < fs->nbase ; i++)
		if (fs->base_keys[i] == key) return true;
	return false;
}

// ---------------------------------------------------------------------------
// names
// ---------------------------------------------------------------------------

static int r40_index(char c)
{
	c = (char)toupper((unsigned char)c);
	for (int i = 0 ; i < 40 ; i++) if (R40_ALPHABET[i] == c) return i;
	return 0;
}

void c5fs_r40_pack(const char *s, unsigned n, uint16_t *out)
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
#define r40_pack c5fs_r40_pack

static void r40_unpack(uint16_t w, char out[3])
{
	out[0] = R40_ALPHABET[(w / 1600) % 40];
	out[1] = R40_ALPHABET[(w / 40) % 40];
	out[2] = R40_ALPHABET[w % 40];
}

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

static struct tracked * track_find(c5fs_t *fs, const char *name, const char *ext)
{
	for (unsigned i = 0 ; i < fs->nfiles ; i++)
		if (!strcmp(fs->files[i].name, name) && !strcmp(fs->files[i].ext, ext))
			return &fs->files[i];
	return NULL;
}

static struct tracked * track_add(c5fs_t *fs)
{
	if (fs->nfiles == fs->cap) {
		unsigned nc = fs->cap ? fs->cap * 2 : 16;
		struct tracked *n = realloc(fs->files, nc * sizeof(*n));
		if (!n) return NULL;
		fs->files = n;
		fs->cap = nc;
	}
	struct tracked *t = &fs->files[fs->nfiles++];
	memset(t, 0, sizeof(*t));
	return t;
}

static void track_del(c5fs_t *fs, struct tracked *t)
{
	unsigned i = (unsigned)(t - fs->files);
	fs->files[i] = fs->files[--fs->nfiles];
}

// ---------------------------------------------------------------------------
// overlay / re-overlay one host file
// ---------------------------------------------------------------------------

static uint32_t image_data_crc(struct c5_area *a, unsigned start, unsigned nsec)
{
	return crc32(area_sec(a, start), (size_t)nsec * SSIZE);
}

// FILDIC word 6 access field (MERA bit numbering N0=MSB): N0-5 hold the
// access category, N6-11 attributes, N12-15 the MEM block size. The
// category is 6 independent bits [owner-r owner-w lower-r lower-w all-r
// all-w]; CROOK's `SET` command names the common combinations. We store
// it human-readably in ".crookfs.ini" as one of those names, defaulting
// to "AW" - everyone read+write, right for a shared exchange disk.
#define C5_ACCESS_MASK    0xfc00u
#define C5_ACCESS_DEFAULT 0xfc00u  // AW / octal 077

static const struct { const char *name; uint16_t bits; } c5_access_names[] = {
	{ "OR", 0x8000 },  // owner: read                    (SET octal 040)
	{ "OW", 0xc000 },  // owner: read+write              (060)
	{ "LR", 0xa000 },  // + subordinate users: read      (050)
	{ "LW", 0xf000 },  // + subordinate users: read+write(074)
	{ "AR", 0xa800 },  // + all users: read              (052)
	{ "AW", 0xfc00 },  // all users: read+write          (077)
};

static const struct { const char *name; uint16_t code; } c5_user_names[] = {
	{ "LIBRAR", C5FS_DICDIC_LIBRAR_WOFF * 4 },        // DICDIC woff 8  -> code 32
	{ "BOSS",   (C5FS_DICDIC_LIBRAR_WOFF + 4) * 4 },  // DICDIC woff 12 -> code 48
};

static uint16_t access_parse(const char *s)
{
	if (!s || !*s) return C5_ACCESS_DEFAULT;
	for (unsigned i = 0 ; i < sizeof(c5_access_names)/sizeof(c5_access_names[0]) ; i++)
		if (!strcasecmp(s, c5_access_names[i].name)) return c5_access_names[i].bits;
	// free-form "owner,lower,all" each "-", "r", "rw"/"rw-"
	unsigned lvl = 0, b[3] = {0, 0, 0};
	for (const char *p = s ; *p && lvl < 3 ; p++) {
		if (*p == ',' || *p == '/') { lvl++; continue; }
		if (*p == 'r' || *p == 'R') b[lvl] |= 1;
		else if (*p == 'w' || *p == 'W') b[lvl] |= 2;
	}
	static const uint8_t rbit[3] = {15, 13, 11};
	uint16_t w = 0;
	for (unsigned i = 0 ; i < 3 ; i++) {
		if (b[i] & 1) w |= 1u << rbit[i];
		if (b[i] & 2) w |= 1u << (rbit[i] - 1);
	}
	return w ? w : C5_ACCESS_DEFAULT;
}

static const char * access_fmt(uint16_t w6)
{
	uint16_t a = w6 & C5_ACCESS_MASK;
	for (unsigned i = 0 ; i < sizeof(c5_access_names)/sizeof(c5_access_names[0]) ; i++)
		if (a == c5_access_names[i].bits) return c5_access_names[i].name;
	static char buf[12];
	snprintf(buf, sizeof(buf), "%s%s,%s%s,%s%s",
		(a & (1u<<15)) ? "r" : "-", (a & (1u<<14)) ? "w" : "-",
		(a & (1u<<13)) ? "r" : "-", (a & (1u<<12)) ? "w" : "-",
		(a & (1u<<11)) ? "r" : "-", (a & (1u<<10)) ? "w" : "-");
	return buf;
}

static bool user_parse(const char *s, uint16_t *out)
{
	if (!s || !*s) return false;
	for (unsigned i = 0 ; i < sizeof(c5_user_names)/sizeof(c5_user_names[0]) ; i++)
		if (!strcasecmp(s, c5_user_names[i].name)) { *out = c5_user_names[i].code; return true; }
	char *end = NULL;
	unsigned long v = strtoul(s, &end, 0);
	if (end == s) return false;
	*out = (uint16_t) v;
	return true;
}

static const char * user_fmt(uint16_t code)
{
	for (unsigned i = 0 ; i < sizeof(c5_user_names)/sizeof(c5_user_names[0]) ; i++)
		if (code == c5_user_names[i].code) return c5_user_names[i].name;
	static char buf[8];
	snprintf(buf, sizeof(buf), "%u", code);
	return buf;
}

static void meta_capture(c5fs_t *fs, const char *hostname, const uint16_t *ent)
{
	if (!fs->meta) return;
	fsmeta_set_s(fs->meta, hostname, "owner",  user_fmt(ent[7]));
	fsmeta_set_s(fs->meta, hostname, "access", access_fmt(ent[6]));
	// attributes / MEM bits are rarely set; keep the raw word only when they are
	if (ent[6] & ~C5_ACCESS_MASK) fsmeta_set_u(fs->meta, hostname, "rights", ent[6]);
	if (ent[5])                   fsmeta_set_u(fs->meta, hostname, "param2", ent[5]);
}

static void meta_apply(c5fs_t *fs, const char *hostname, uint16_t *ent)
{
	unsigned long v;
	uint16_t u;
	if (user_parse(fsmeta_get_s(fs->meta, hostname, "owner"), &u)) { ent[2] = u; ent[7] = u; }
	if (fsmeta_get_u(fs->meta, hostname, "param2", &v)) ent[5] = (uint16_t) v;
	// "rights" (raw word 6) wins if given; else "access" name/triad; else default
	if (fsmeta_get_u(fs->meta, hostname, "rights", &v)) {
		ent[6] = (uint16_t) v;
	} else {
		const char *acc = fsmeta_get_s(fs->meta, hostname, "access");
		ent[6] = (ent[6] & ~C5_ACCESS_MASK) | access_parse(acc);
	}
	if (fsmeta_get_u(fs->meta, hostname, "word7",  &v)) ent[7] = (uint16_t) v;
}

// Slurp a host file. CROOK-5 sequential text files use CR+LF line ends (the
// character I/O layer terminates records on CR, code 13); a bare-LF host file
// makes LIST read past the end of every line. So when the file looks like text
// (no NUL bytes, no CR already present) translate every LF to CR+LF here, and
// reverse it on write-back. `*is_text` reports whether that happened. Caller
// frees `*out`.
static bool load_host_file(const char *path, long host_size,
                           uint8_t **out, size_t *out_len, bool *is_text)
{
	*out = NULL; *out_len = 0; *is_text = false;
	FILE *f = fopen(path, "rb");
	if (!f) return false;

	uint8_t *raw = malloc((size_t)host_size ? (size_t)host_size : 1);
	if (!raw) { fclose(f); return false; }
	size_t got = fread(raw, 1, (size_t)host_size, f);
	fclose(f);
	if (got != (size_t)host_size) { free(raw); return false; }

	bool text = host_size > 0;
	unsigned lf = 0;
	for (long i = 0 ; i < host_size ; i++) {
		if (raw[i] == 0 || raw[i] == '\r') { text = false; break; }
		if (raw[i] == '\n') lf++;
	}
	if (!text || lf == 0) { *out = raw; *out_len = got; return true; }

	uint8_t *conv = malloc(got + lf);
	if (!conv) { *out = raw; *out_len = got; return true; }
	size_t o = 0;
	for (size_t i = 0 ; i < got ; i++) {
		if (raw[i] == '\n') conv[o++] = '\r';
		conv[o++] = raw[i];
	}
	free(raw);
	*out = conv; *out_len = o; *is_text = true;
	return true;
}

static bool overlay_file(struct c5_area *a, const char *host_path, const char *fn,
                         const struct stat *st)
{
	c5fs_t *fs = a->fs;
	if (!S_ISREG(st->st_mode) || st->st_size > MAX_FILE_BYTES) return false;

	char name[MAX_NAME + 1], ext[MAX_EXT + 1];
	split_name(fn, name, ext);
	uint16_t nw[2] = {0, 0}, ew = 0;
	r40_pack(name, MAX_NAME, nw);
	r40_pack(ext, MAX_EXT, &ew);

	uint8_t *content = NULL;
	size_t clen = 0;
	bool is_text = false;
	if (!load_host_file(host_path, (long)st->st_size, &content, &clen, &is_text))
		return false;

	unsigned nsec = (unsigned)((clen + SSIZE - 1) / SSIZE);
	if (nsec == 0) nsec = 1;

	struct tracked *old = track_find(fs, name, ext);
	if (old || fildic_find(a, nw[0], nw[1]))
		fildic_remove(a, nw[0], nw[1]);

	long start = map_find_run(a, nsec);
	if (start < 0) {
		LOGWARN("crook5fs: no room for \"%s\" (%u sectors)", fn, nsec);
		free(content);
		if (old) track_del(fs, old);
		return false;
	}

	uint8_t *dst = area_sec(a, (unsigned)start);
	memset(dst, 0, (size_t)nsec * SSIZE);
	memcpy(dst, content, clen);
	free(content);
	for (unsigned i = 0 ; i < nsec ; i++) map_set(a, (unsigned)start + i);

	uint16_t p2 = 0, w6 = C5_ACCESS_DEFAULT;
	fildic_clone_template(a, ew, &p2, &w6);
	// take only the attribute / MEM bits from a same-ext template; the
	// access category is our own policy (default AW, or the .crookfs.ini
	// "access =" line), applied below by meta_apply().
	w6 = C5_ACCESS_DEFAULT | (w6 & ~C5_ACCESS_MASK);

	uint16_t ent[FILDIC_ENTRY_WORDS] = {0};
	ent[0] = nw[0]; ent[1] = nw[1];
	ent[2] = DICDIC_LIBRAR_CODE;
	ent[3] = ew;
	// param1 / param2 pin the byte-exact end of file for CROOK's character
	// I/O layer (opcr57: "parametr 1 - względny adres byte'owy ostatniego
	// znaku zbioru względem końca sektora, parametr 2 - numer ostatniego
	// sektora względem początku zbioru"). Every real CROOK data file carries
	// them; without them LIST reads a whole trailing sector of padding as
	// extra lines. param1 is stored as a negative "bytes short of a full
	// last sector" count (matches CFA/PER output on the p8f reference disk).
	ent[4] = (uint16_t)(int16_t)((long)clen - (long)nsec * SSIZE);
	ent[5] = (uint16_t)(nsec - 1);
	(void) p2;
	ent[6] = w6;
	ent[7] = DICDIC_LIBRAR_CODE;
	ent[8] = 0;
	ent[9]  = (uint16_t)start;
	ent[10] = (uint16_t)(start + nsec);
	ent[11] = (uint16_t)nsec;

	meta_apply(fs, fn, ent);

	if (!fildic_insert(a, ent)) {
		for (unsigned i = 0 ; i < nsec ; i++) map_clear(a, (unsigned)start + i);
		LOGWARN("crook5fs: FILDIC full, dropped \"%s\"", fn);
		if (old) track_del(fs, old);
		return false;
	}

	struct tracked *t = old ? old : track_add(fs);
	if (!t) return false;
	snprintf(t->name, sizeof(t->name), "%s", name);
	snprintf(t->ext, sizeof(t->ext), "%s", ext);
	t->w0 = nw[0]; t->w1 = nw[1]; t->ew = ew;
	t->start = (unsigned)start; t->nsec = nsec;
	t->host_size = st->st_size;
	t->host_mtime = (long)st->st_mtime;
	t->text_crlf = is_text;
	t->crc = image_data_crc(a, (unsigned)start, nsec);
	meta_capture(fs, fn, ent);
	LOG(fs->logc, "crook5fs: host->guest  %s.%s  %u sect @ %ld", name, ext, nsec, start);
	return true;
}

// ---------------------------------------------------------------------------
// host -> guest
// ---------------------------------------------------------------------------

struct scan_ent { char raw[512]; char name[MAX_NAME + 1], ext[MAX_EXT + 1]; struct stat st; };

static void host_to_guest(c5fs_t *fs, struct c5_area *a)
{
	DIR *dp = opendir(fs->dir_name);
	if (!dp) return;

	struct scan_ent *list = NULL;
	unsigned n = 0, cap = 0;
	struct dirent *de;
	while ((de = readdir(dp)) != NULL) {
		if (de->d_name[0] == '.') continue;
		char path[2048];
		snprintf(path, sizeof(path), "%s/%s", fs->dir_name, de->d_name);
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

	for (unsigned i = 0 ; i < fs->nfiles ; ) {
		struct tracked *t = &fs->files[i];
		bool on_host = false;
		for (unsigned j = 0 ; j < n ; j++)
			if (!strcmp(list[j].name, t->name) && !strcmp(list[j].ext, t->ext)) { on_host = true; break; }
		if (!on_host) {
			char gone[64];
			host_basename(t, gone, sizeof(gone));
			fsmeta_forget(fs->meta, gone);
			LOG(fs->logc, "crook5fs: host->guest  removed %s.%s", t->name, t->ext);
			fildic_remove(a, t->w0, t->w1);
			track_del(fs, t);
			continue;
		}
		i++;
	}

	for (unsigned j = 0 ; j < n ; j++) {
		struct tracked *t = track_find(fs, list[j].name, list[j].ext);
		if (!t || t->host_mtime != (long)list[j].st.st_mtime || t->host_size != list[j].st.st_size) {
			char path[2048];
			snprintf(path, sizeof(path), "%s/%s", fs->dir_name, list[j].raw);
			overlay_file(a, path, list[j].raw, &list[j].st);
		}
	}
	free(list);
	fsmeta_save(fs->meta);
}

// ---------------------------------------------------------------------------
// guest -> host
// ---------------------------------------------------------------------------

static void write_host_file(c5fs_t *fs, struct c5_area *a, struct tracked *t,
                            const uint16_t *ent)
{
	char base[64], path[2048];
	host_basename(t, base, sizeof(base));
	snprintf(path, sizeof(path), "%s/%s", fs->dir_name, base);

	unsigned start = ent[9], nsec = ent[11];
	long len = (long)nsec * SSIZE;
	// param1 is a negative "bytes short of the full last sector" count that
	// marks EOF for character I/O (see overlay_file / the p8f reference disk).
	int16_t p1 = (int16_t) ent[4];
	if (p1 < 0 && -p1 < len) len += p1;

	const uint8_t *src = area_sec(a, start);

	FILE *f = fopen(path, "wb");
	if (!f) { LOGWARN("crook5fs: cannot write back \"%s\"", path); return; }
	if (t->text_crlf) {
		// undo the LF->CRLF done on the way in: drop trailing zero padding,
		// then drop every CR that sits right before an LF.
		while (len > 0 && src[len - 1] == 0) len--;
		for (long i = 0 ; i < len ; i++) {
			if (src[i] == '\r' && i + 1 < len && src[i + 1] == '\n') continue;
			fputc(src[i], f);
		}
	} else {
		fwrite(src, 1, (size_t)len, f);
	}
	fclose(f);

	meta_capture(fs, base, ent);

	struct stat st;
	t->start = start; t->nsec = nsec;
	t->crc = image_data_crc(a, start, nsec);
	t->host_size = len;
	if (stat(path, &st) == 0) t->host_mtime = (long)st.st_mtime;
	LOG(fs->logc, "crook5fs: guest->host  %s (%ld bytes)", base, len);
}

static void guest_to_host(c5fs_t *fs, struct c5_area *a)
{
	for (unsigned i = 0 ; i < fs->nfiles ; ) {
		struct tracked *t = &fs->files[i];
		uint16_t *ent = fildic_find(a, t->w0, t->w1);
		if (!ent) {
			char base[64], path[2048];
			host_basename(t, base, sizeof(base));
			snprintf(path, sizeof(path), "%s/%s", fs->dir_name, base);
			remove(path);
			fsmeta_forget(fs->meta, base);
			LOG(fs->logc, "crook5fs: guest->host  deleted %s", base);
			track_del(fs, t);
			continue;
		}
		if (image_data_crc(a, ent[9], ent[11]) != t->crc)
			write_host_file(fs, a, t, ent);
		i++;
	}

	for (unsigned rel = 0 ; rel < a->fildic_sectors ; rel++) {
		uint8_t *sec = area_sec(a, a->A1 + rel);
		for (unsigned e = 0 ; e < FILDIC_SLOTS ; e++) {
			unsigned o = e * FILDIC_ENTRY_WORDS;
			uint16_t w0 = rdw(sec, o);
			if (w0 == 1) break;
			if (w0 == 0) continue;
			if (rdw(sec, o + 2) != DICDIC_LIBRAR_CODE) continue;
			uint16_t w1 = rdw(sec, o + 1), ew = rdw(sec, o + 3);
			if (is_base_file(fs, w0, w1, ew)) continue;

			char name[MAX_NAME + 1], ext[MAX_EXT + 1];
			r40_words_to_str(w0, w1, ew, name, ext);
			if (track_find(fs, name, ext)) continue;

			struct tracked *t = track_add(fs);
			if (!t) return;
			snprintf(t->name, sizeof(t->name), "%s", name);
			snprintf(t->ext, sizeof(t->ext), "%s", ext);
			t->w0 = w0; t->w1 = w1; t->ew = ew;
			t->crc = ~image_data_crc(a, rdw(sec, o + 9), rdw(sec, o + 11));
			uint16_t ent[FILDIC_ENTRY_WORDS];
			for (unsigned k = 0 ; k < FILDIC_ENTRY_WORDS ; k++) ent[k] = rdw(sec, o + k);
			write_host_file(fs, a, t, ent);
		}
	}

	fsmeta_save(fs->meta);
}

// ---------------------------------------------------------------------------
// sync scheduling
// ---------------------------------------------------------------------------

static void on_host_change(void *user)
{
	c5fs_t *fs = user;
	atomic_store(&fs->host_evt, (long long) now_secs());
	atomic_store(&fs->host_dirty, true);
}

static void maybe_sync(c5fs_t *fs)
{
	if (!fs->sync) return;
	long t = now_secs();

	if (fs->guest_dirty && (t - fs->last_write) >= SYNC_QUIESCE_SECS) {
		struct c5_area a;
		if (area_open(&a, fs, fs->area_start)) guest_to_host(fs, &a);
		fs->guest_dirty = false;
		fs->last_poll = t;
		return;
	}
	if (fs->guest_dirty) return;

	if (fs->watch) {
		if (atomic_load(&fs->host_dirty)
		    && (t - (long) atomic_load(&fs->host_evt)) >= SYNC_QUIESCE_SECS) {
			atomic_store(&fs->host_dirty, false);
			struct c5_area a;
			if (area_open(&a, fs, fs->area_start)) host_to_guest(fs, &a);
			fs->last_poll = t;
		}
	} else if ((t - fs->last_poll) >= POLL_INTERVAL_SECS) {
		struct c5_area a;
		if (area_open(&a, fs, fs->area_start)) host_to_guest(fs, &a);
		fs->last_poll = t;
	}
}

// ---------------------------------------------------------------------------
// initial overlay
// ---------------------------------------------------------------------------

bool c5fs_librar_present(struct c5_area *a)
{
	uint8_t *d0 = area_sec(a, a->A0);
	char nm[6];
	r40_unpack(rdw(d0, DICDIC_LIBRAR_WOFF), nm);
	r40_unpack(rdw(d0, DICDIC_LIBRAR_WOFF + 1), nm + 3);
	return strncmp(nm, "LIBRAR", 6) == 0;
}
#define librar_present c5fs_librar_present

static void overlay_dir(c5fs_t *fs)
{
	struct c5_area a;
	if (!area_open(&a, fs, fs->area_start) || !librar_present(&a)) {
		LOGWARN("crook5fs: area has no usable LIBRAR - directory not merged.");
		return;
	}

	fildic_snapshot_base(fs, &a);

	DIR *dp = opendir(fs->dir_name);
	if (!dp) {
		LOGWARN("crook5fs: cannot open \"%s\" - serving the area as-is.", fs->dir_name);
		return;
	}
	unsigned added = 0;
	struct dirent *de;
	while ((de = readdir(dp)) != NULL) {
		if (de->d_name[0] == '.') continue;
		char path[2048];
		snprintf(path, sizeof(path), "%s/%s", fs->dir_name, de->d_name);
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
		if (overlay_file(&a, path, de->d_name, &st)) added++;
	}
	closedir(dp);

	fs->sync = true;
	fs->last_poll = now_secs();
	fsmeta_save(fs->meta);
	LOG(fs->logc, "crook5fs: merged %u file(s) from \"%s\" into LIBRAR (two-way sync on)",
		added, fs->dir_name);
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

c5fs_t * c5fs_create(uint8_t *mem, long size, unsigned spare, int logc)
{
	c5fs_t *fs = calloc(1, sizeof(*fs));
	if (!fs) return NULL;
	fs->mem = mem;
	fs->size = size;
	fs->spare = spare;
	fs->logc = logc;
	return fs;
}

void c5fs_mount_dir(c5fs_t *fs, const char *dir_name, unsigned area_start,
                    const char *sidecar)
{
	if (!fs || !dir_name || !*dir_name) return;
	fs->dir_name = strdup(dir_name);
	fs->area_start = area_start;
	fs->meta = fsmeta_load(fs->dir_name, sidecar);
	overlay_dir(fs);
	if (fs->sync) {
		fs->watch = dirwatch_create(fs->dir_name, on_host_change, fs);
		LOG(fs->logc, "crook5fs: host watch %s", fs->watch ? "active" : "unavailable, polling");
	}
}

void c5fs_flush(c5fs_t *fs)
{
	if (!fs) return;
	if (fs->watch) { dirwatch_destroy(fs->watch); fs->watch = NULL; }
	if (fs->sync) {
		struct c5_area a;
		if (area_open(&a, fs, fs->area_start)) guest_to_host(fs, &a);
	}
	fsmeta_save(fs->meta);
}

void c5fs_destroy(c5fs_t *fs)
{
	if (!fs) return;
	if (fs->watch) dirwatch_destroy(fs->watch);
	fsmeta_free(fs->meta);
	free(fs->base_keys);
	free(fs->files);
	free(fs->dir_name);
	free(fs);
}

void c5fs_on_read(c5fs_t *fs)  { if (fs) maybe_sync(fs); }
void c5fs_on_write(c5fs_t *fs)
{
	if (fs && fs->sync) { fs->guest_dirty = true; fs->last_write = now_secs(); }
}

// vim: tabstop=4 shiftwidth=4 autoindent
