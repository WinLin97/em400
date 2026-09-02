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

#include "log.h"
#include "io/dev/fsmeta.h"

#define FILE_MAX  64
#define KEY_MAX   24
#define VAL_MAX   64
#define ENT_GROW  32

struct ent {
	char file[FILE_MAX];
	char key[KEY_MAX];
	char val[VAL_MAX];
};

struct fsmeta {
	char *path;
	struct ent *e;
	unsigned n, cap;
	bool dirty;
};

// -----------------------------------------------------------------------
static void trim(char *s)
{
	char *p = s;
	while (*p && isspace((unsigned char) *p)) p++;
	if (p != s) memmove(s, p, strlen(p) + 1);
	size_t l = strlen(s);
	while (l && isspace((unsigned char) s[l - 1])) s[--l] = 0;
}

// -----------------------------------------------------------------------
static struct ent * find(fsmeta_t *m, const char *file, const char *key)
{
	for (unsigned i = 0 ; i < m->n ; i++)
		if (!strcmp(m->e[i].file, file) && !strcmp(m->e[i].key, key))
			return &m->e[i];
	return NULL;
}

// -----------------------------------------------------------------------
static void set_raw(fsmeta_t *m, const char *file, const char *key, const char *val)
{
	struct ent *e = find(m, file, key);
	if (!e) {
		if (m->n == m->cap) {
			unsigned nc = m->cap + ENT_GROW;
			struct ent *ne = realloc(m->e, nc * sizeof(*ne));
			if (!ne) return;
			m->e = ne;
			m->cap = nc;
		}
		e = &m->e[m->n++];
		snprintf(e->file, sizeof(e->file), "%s", file);
		snprintf(e->key, sizeof(e->key), "%s", key);
		e->val[0] = 0;
	}
	if (strcmp(e->val, val)) {
		snprintf(e->val, sizeof(e->val), "%s", val);
		m->dirty = true;
	}
}

// -----------------------------------------------------------------------
static void parse(fsmeta_t *m, FILE *f)
{
	char line[256];
	char cur[FILE_MAX] = {0};
	while (fgets(line, sizeof(line), f)) {
		trim(line);
		if (!line[0] || line[0] == '#' || line[0] == ';') continue;
		if (line[0] == '[') {
			char *end = strchr(line, ']');
			if (!end) continue;
			*end = 0;
			snprintf(cur, sizeof(cur), "%s", line + 1);
			trim(cur);
			continue;
		}
		if (!cur[0]) continue;
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		char *key = line, *val = eq + 1;
		trim(key);
		trim(val);
		if (key[0]) set_raw(m, cur, key, val);
	}
	m->dirty = false;   // just loaded, matches disk
}

// -----------------------------------------------------------------------
fsmeta_t * fsmeta_load(const char *dir, const char *basename)
{
	fsmeta_t *m = calloc(1, sizeof(*m));
	if (!m) return NULL;

	size_t need = strlen(dir) + 1 + strlen(basename) + 1;
	m->path = malloc(need);
	if (!m->path) { free(m); return NULL; }
	snprintf(m->path, need, "%s/%s", dir, basename);

	FILE *f = fopen(m->path, "r");
	if (f) {
		parse(m, f);
		fclose(f);
		LOG(L_WNCH, "fsmeta: loaded %u attribute(s) from \"%s\"", m->n, m->path);
	}
	return m;
}

// -----------------------------------------------------------------------
void fsmeta_save(fsmeta_t *m)
{
	if (!m || !m->dirty) return;

	if (m->n == 0) {
		remove(m->path);
		m->dirty = false;
		return;
	}

	FILE *f = fopen(m->path, "w");
	if (!f) {
		LOGWARN("fsmeta: cannot write \"%s\"", m->path);
		return;
	}
	fprintf(f, "# em400 directory-backed disk - per-file attributes (auto-generated)\n");

	// emit grouped by file, in first-seen order
	char done[FILE_MAX];
	for (unsigned i = 0 ; i < m->n ; i++) {
		bool seen = false;
		for (unsigned j = 0 ; j < i ; j++)
			if (!strcmp(m->e[j].file, m->e[i].file)) { seen = true; break; }
		if (seen) continue;
		snprintf(done, sizeof(done), "%s", m->e[i].file);
		fprintf(f, "\n[%s]\n", done);
		for (unsigned k = i ; k < m->n ; k++)
			if (!strcmp(m->e[k].file, done))
				fprintf(f, "%s = %s\n", m->e[k].key, m->e[k].val);
	}
	fclose(f);
	m->dirty = false;
	LOG(L_WNCH, "fsmeta: saved \"%s\"", m->path);
}

// -----------------------------------------------------------------------
void fsmeta_free(fsmeta_t *m)
{
	if (!m) return;
	free(m->e);
	free(m->path);
	free(m);
}

// -----------------------------------------------------------------------
bool fsmeta_get_u(fsmeta_t *m, const char *file, const char *key, unsigned long *out)
{
	if (!m) return false;
	struct ent *e = find(m, file, key);
	if (!e) return false;
	char *end = NULL;
	unsigned long v = strtoul(e->val, &end, 0);
	if (end == e->val) return false;
	if (out) *out = v;
	return true;
}

// -----------------------------------------------------------------------
void fsmeta_set_u(fsmeta_t *m, const char *file, const char *key, unsigned long val)
{
	if (!m) return;
	char buf[VAL_MAX];
	// masks read better in hex, small counts in decimal
	if (val > 0xff) snprintf(buf, sizeof(buf), "0x%04lx", val);
	else            snprintf(buf, sizeof(buf), "%lu", val);
	set_raw(m, file, key, buf);
}

// -----------------------------------------------------------------------
void fsmeta_forget(fsmeta_t *m, const char *file)
{
	if (!m) return;
	for (unsigned i = 0 ; i < m->n ; ) {
		if (!strcmp(m->e[i].file, file)) {
			m->e[i] = m->e[--m->n];
			m->dirty = true;
		} else {
			i++;
		}
	}
}

// vim: tabstop=4 shiftwidth=4 autoindent
