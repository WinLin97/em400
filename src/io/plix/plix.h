//  Copyright (c) 2026 Marcin Golesz
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

#ifndef IO_PLIX_H
#define IO_PLIX_H

#include <stdbool.h>

#include "io/chan.h"

chan_t * plix_create(int chnum);
bool plix_dev_compatible(int dev_type);

#endif

// vim: tabstop=4 shiftwidth=4 autoindent
