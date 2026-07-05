//  Copyright (c) 2016 Jakub Filipowicz <jakubf@gmail.com>
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

#ifndef UI_H
#define UI_H

#include <stdio.h>

#include "libem400.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void * (*ui_f_setup)(const char *);
typedef int (*ui_f_poweron)(void *, const char *);
typedef void (*ui_f_loop)(void*);
typedef void (*ui_f_poweroff)(void*);
typedef void (*ui_f_destroy)(void*);
typedef void (*ui_f_msg)(void *, em400_sev_t, const char *);

struct ui_drv {
	const char *name;
	ui_f_setup setup;
	ui_f_poweron poweron;
	ui_f_loop loop;
	ui_f_poweroff poweroff;
	ui_f_destroy destroy;
	ui_f_msg msg;
};

struct ui {
	struct ui_drv *drv;
	void *data;
};

void ui_print_uis(FILE *fd);
struct ui * ui_create(const char *name);
int ui_run(struct ui *ui, const char *program);
void ui_shutdown(struct ui *ui);

void ui_msg(em400_sev_t sev, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif

// vim: tabstop=4 shiftwidth=4 autoindent
