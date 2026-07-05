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

#define _XOPEN_SOURCE 500
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <assert.h>
#include <pthread.h>

#include "libem400.h"
#include "ui/ui.h"

extern struct ui_drv ui_cmd;
extern struct ui_drv ui_qt6;

static struct ui *active_ui;
// the message sink may pop GUI widgets, so it must run on the UI thread. Captured
// in ui_create (which runs on it); ui_msg falls back to stderr from any other thread.
static pthread_t ui_thread;
static bool ui_thread_set;

// in order of preference
struct ui_drv* uis[] = {
#ifdef UI_QT
	&ui_qt6,
#endif
	&ui_cmd,
	NULL
};

// -----------------------------------------------------------------------
void ui_print_uis(FILE *fd)
{
	struct ui_drv **u = uis;
	while (*u) {
		if (u != uis) fprintf(fd, ",");
		fprintf(fd, " %s", (*u)->name);
		u++;
	}
}

// -----------------------------------------------------------------------
struct ui * ui_create(const char *name)
{
	if (!name) {
		name = uis[0]->name;
	}

	struct ui_drv **drv = uis;
	while (drv && *drv) {
		if (!strncasecmp(name, (*drv)->name, strlen((*drv)->name))) {
			struct ui *ui = (struct ui *) calloc(1, sizeof(struct ui));
			if (!ui) {
				em400_msg(EM400_MSG_ERROR, "Memory allocation error when creating UI.");
				return NULL;
			}
			ui->drv = *drv;

			// setup the UI
			ui->data = ui->drv->setup(name);
			if (!ui->data) {
				em400_msg(EM400_MSG_ERROR, "Failed to setup UI: %s.", name);
				free(ui);
				return NULL;
			}
			ui_thread = pthread_self();
			ui_thread_set = true;
			active_ui = ui;
			return ui;
		}
		drv++;
	}

	em400_msg(EM400_MSG_ERROR, "Unknown UI: %s.", name);
	return NULL;
}

// -----------------------------------------------------------------------
int ui_run(struct ui *ui, const char *program)
{
	if (ui->drv->poweron && ui->drv->poweron(ui->data, program) != E_OK) {
		return E_ERR;
	}

	ui->drv->loop(ui->data);

	return E_OK;
}

// -----------------------------------------------------------------------
void ui_shutdown(struct ui *ui)
{
	if (!ui) {
		return;
	}

	if (ui->drv->poweroff) {
		ui->drv->poweroff(ui->data);
	}
	if (ui == active_ui) {
		active_ui = NULL;
	}
	ui->drv->destroy(ui->data);
	free(ui);
}

// -----------------------------------------------------------------------
static void ui_msg_stderr(em400_sev_t sev, const char *text)
{
	const char *prefix = "";
	if (sev == EM400_MSG_ERROR) {
		prefix = "ERROR: ";
	} else if (sev == EM400_MSG_WARNING) {
		prefix = "WARNING: ";
	}
	fprintf(stderr, "%s%s\n", prefix, text);
}

// -----------------------------------------------------------------------
void ui_msg(em400_sev_t sev, const char *fmt, ...)
{
	char buf[512];

	va_list vl;
	va_start(vl, fmt);
	vsnprintf(buf, sizeof buf, fmt, vl);
	va_end(vl);

	bool on_ui_thread = ui_thread_set && pthread_equal(pthread_self(), ui_thread);
	// once the UI thread exists, user-facing messages must originate on it;
	// not-yet-set means early startup (pre-ui_create), which stderr handles.
	assert((!ui_thread_set || on_ui_thread) && "ui_msg called off the UI thread");

	if (active_ui && active_ui->drv->msg && on_ui_thread) {
		active_ui->drv->msg(active_ui->data, sev, buf);
	} else {
		ui_msg_stderr(sev, buf);
	}
}

// vim: tabstop=4 shiftwidth=4 autoindent
