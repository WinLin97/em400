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

#ifndef FSMETA_H
#define FSMETA_H

#include <stdbool.h>

// Prototype metadata sidecar for the directory-backed disk drives.
//
// A host file carries only name + mtime + size + bytes. A CROOK-5 (or IBM 3740)
// directory entry carries more: owner code, rights bits, record length, file
// type. Those attributes would be lost on a host->guest->host round trip, or
// reset to defaults on the next mount. This sidecar - a small INI file in the
// mount directory (its name starts with '.', so the overlay never treats it as
// a file) - remembers them keyed by host filename:
//
//   [HELLO.BAS]
//   owner  = 32
//   rights = 0xa800
//   param2 = 4
//
// Prototype: flat in-memory key/value list, whole file rewritten on save,
// numeric values parsed with strtoul base 0. Good enough to preserve a handful
// of attributes per file; not a general config store.

typedef struct fsmeta fsmeta_t;

// Load "<dir>/<basename>" if present (missing file -> empty store, never NULL
// unless out of memory).
fsmeta_t * fsmeta_load(const char *dir, const char *basename);

// Rewrite the sidecar if anything changed since load. Drops the file entirely
// when the store is empty.
void fsmeta_save(fsmeta_t *m);
void fsmeta_free(fsmeta_t *m);

// Per-file attribute access. `file` is the host basename, `key` a short token.
bool fsmeta_get_u(fsmeta_t *m, const char *file, const char *key, unsigned long *out);
void fsmeta_set_u(fsmeta_t *m, const char *file, const char *key, unsigned long val);
// String-valued attributes (e.g. a human-readable "access = rw,rw,rw").
const char * fsmeta_get_s(fsmeta_t *m, const char *file, const char *key);
void fsmeta_set_s(fsmeta_t *m, const char *file, const char *key, const char *val);

// Forget every attribute of one file (e.g. it was deleted).
void fsmeta_forget(fsmeta_t *m, const char *file);

#endif

// vim: tabstop=4 shiftwidth=4 autoindent
