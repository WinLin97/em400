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
#include <string.h>
#include <uv.h>

#include "log.h"
#include "io/dev/dirwatch.h"

// One dedicated libuv loop + thread for every directory watch in the process.
// It is deliberately NOT em400's I/O loop (uv_default_loop()): watches must not
// be tangled with device/channel teardown, and this loop can outlive a machine
// reset. Registration and removal are marshalled onto the loop thread through
// `wasync` + a mutex-guarded op queue, because libuv handles may only be touched
// from their loop's thread.

#define FALLBACK_POLL_MS 2000

struct dirwatch {
	uv_fs_event_t ev;
	uv_timer_t timer;
	char *path;
	void (*cb)(void *user);
	void *user;
	int closing_left;                 // handles still to close before free (loop thread only)
	uv_sem_t *done;                   // posted once the struct is fully torn down
};

enum op_kind { OP_ADD, OP_DEL };
struct op {
	struct dirwatch *dw;
	enum op_kind kind;
	struct op *next;
};

static uv_loop_t wloop;
static uv_thread_t wthread;
static uv_async_t wasync;
static uv_mutex_t wlock;
static uv_once_t wonce = UV_ONCE_INIT;
static bool wrunning;
static struct op *wq_head, *wq_tail;

// -----------------------------------------------------------------------
static void once_init(void)
{
	uv_mutex_init(&wlock);
}

// -----------------------------------------------------------------------
static void wthread_fn(void *arg)
{
	(void) arg;
	uv_run(&wloop, UV_RUN_DEFAULT);
}

// -----------------------------------------------------------------------
static void on_fs_event(uv_fs_event_t *h, const char *filename, int events, int status)
{
	(void) filename; (void) events; (void) status;
	struct dirwatch *dw = uv_handle_get_data((uv_handle_t *) h);
	if (dw && dw->cb) dw->cb(dw->user);
}

// -----------------------------------------------------------------------
static void on_timer(uv_timer_t *h)
{
	struct dirwatch *dw = uv_handle_get_data((uv_handle_t *) h);
	if (dw && dw->cb) dw->cb(dw->user);
}

// -----------------------------------------------------------------------
static void free_dirwatch(struct dirwatch *dw)
{
	uv_sem_t *done = dw->done;
	free(dw->path);
	free(dw);
	if (done) uv_sem_post(done);
}

// -----------------------------------------------------------------------
static void on_handle_closed(uv_handle_t *h)
{
	struct dirwatch *dw = uv_handle_get_data(h);
	if (dw && --dw->closing_left == 0) free_dirwatch(dw);
}

// -----------------------------------------------------------------------
static void process_queue(uv_async_t *a)
{
	(void) a;

	struct op *ops;
	uv_mutex_lock(&wlock);
	ops = wq_head;
	wq_head = wq_tail = NULL;
	uv_mutex_unlock(&wlock);

	while (ops) {
		struct op *o = ops;
		ops = o->next;
		struct dirwatch *dw = o->dw;

		if (o->kind == OP_ADD) {
			uv_fs_event_init(&wloop, &dw->ev);
			uv_handle_set_data((uv_handle_t *) &dw->ev, dw);
			uv_timer_init(&wloop, &dw->timer);
			uv_handle_set_data((uv_handle_t *) &dw->timer, dw);

			// flags = 0: the mount dirs are flat; UV_FS_EVENT_RECURSIVE is
			// unsupported on Linux and would force everyone onto the fallback.
			int r = uv_fs_event_start(&dw->ev, on_fs_event, dw->path, 0);
			if (r < 0) {
				LOGWARN("dirwatch: no native fs-events for \"%s\" (%s); polling every %dms",
					dw->path, uv_strerror(r), FALLBACK_POLL_MS);
				uv_close((uv_handle_t *) &dw->ev, NULL);
				uv_timer_start(&dw->timer, on_timer, FALLBACK_POLL_MS, FALLBACK_POLL_MS);
			}
		} else {
			dw->closing_left = 0;
			if (!uv_is_closing((uv_handle_t *) &dw->ev))    { dw->closing_left++; uv_close((uv_handle_t *) &dw->ev, on_handle_closed); }
			if (!uv_is_closing((uv_handle_t *) &dw->timer)) { dw->closing_left++; uv_close((uv_handle_t *) &dw->timer, on_handle_closed); }
			if (dw->closing_left == 0) free_dirwatch(dw);
		}
		free(o);
	}
}

// -----------------------------------------------------------------------
// wlock held
static bool ensure_running(void)
{
	if (wrunning) return true;

	if (uv_loop_init(&wloop)) return false;
	if (uv_async_init(&wloop, &wasync, process_queue)) {
		uv_loop_close(&wloop);
		return false;
	}
	if (uv_thread_create(&wthread, wthread_fn, NULL)) {
		uv_close((uv_handle_t *) &wasync, NULL);
		uv_run(&wloop, UV_RUN_NOWAIT);
		uv_loop_close(&wloop);
		return false;
	}
	wrunning = true;
	return true;
}

// -----------------------------------------------------------------------
static void queue_op(struct dirwatch *dw, enum op_kind kind)
{
	struct op *o = calloc(1, sizeof(*o));
	if (!o) return;
	o->dw = dw;
	o->kind = kind;

	uv_mutex_lock(&wlock);
	if (wq_tail) wq_tail->next = o; else wq_head = o;
	wq_tail = o;
	uv_mutex_unlock(&wlock);

	uv_async_send(&wasync);
}

// -----------------------------------------------------------------------
dirwatch_t * dirwatch_create(const char *path, void (*cb)(void *user), void *user)
{
	if (!path || !*path || !cb) return NULL;
	uv_once(&wonce, once_init);

	struct dirwatch *dw = calloc(1, sizeof(*dw));
	if (!dw) return NULL;
	dw->path = strdup(path);
	dw->cb = cb;
	dw->user = user;
	if (!dw->path) { free(dw); return NULL; }

	uv_mutex_lock(&wlock);
	bool ok = ensure_running();
	uv_mutex_unlock(&wlock);
	if (!ok) {
		LOGWARN("dirwatch: could not start watcher thread; \"%s\" left to polling", path);
		free(dw->path);
		free(dw);
		return NULL;
	}

	queue_op(dw, OP_ADD);
	return dw;
}

// -----------------------------------------------------------------------
void dirwatch_destroy(dirwatch_t *dw)
{
	if (!dw) return;

	// Block until the watcher thread has closed the handles and freed the
	// struct, so the owner can safely free itself right after this returns.
	uv_sem_t done;
	uv_sem_init(&done, 0);
	dw->done = &done;
	dw->cb = NULL;                     // no more callbacks into a dying owner

	queue_op(dw, OP_DEL);

	uv_sem_wait(&done);
	uv_sem_destroy(&done);
}

// vim: tabstop=4 shiftwidth=4 autoindent
