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

#ifndef DIRWATCH_H
#define DIRWATCH_H

// Watch a host directory for changes, used by the directory-backed disk drives
// to notice host-side edits without polling on the emulation path.
//
// A single libuv loop on a dedicated thread (separate from em400's I/O loop)
// serves all watches. `cb` runs on that thread - keep it short and thread-safe
// (typically: set an atomic flag the sector path reads). It fires on any change
// under `path`, and also periodically as a fallback when the platform has no
// native filesystem events (network shares, some FUSE mounts).

typedef struct dirwatch dirwatch_t;

dirwatch_t * dirwatch_create(const char *path, void (*cb)(void *user), void *user);
void dirwatch_destroy(dirwatch_t *dw);

#endif

// vim: tabstop=4 shiftwidth=4 autoindent
