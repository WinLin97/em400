//  Copyright (c) 2020-2026 Jakub Filipowicz <jakubf@gmail.com>
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

#define MAX_KEY_LEN 128

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"

// -----------------------------------------------------------------------
const char * cfg_default_log_file()
{
#ifdef _WIN32
	// CWD-relative em400.log would land in the install dir (Program Files,
	// not user-writable), so default next to the config in %APPDATA%
	static char *path;
	if (!path) {
		const char *appdata = getenv("APPDATA");
		if (appdata && *appdata) {
			size_t len = strlen(appdata) + strlen("\\em400\\" CFG_DEFAULT_LOG_FILE) + 1;
			path = (char *) malloc(len);
			if (path) {
				snprintf(path, len, "%s\\em400\\%s", appdata, CFG_DEFAULT_LOG_FILE);
			}
		}
	}
	return path ? path : CFG_DEFAULT_LOG_FILE;
#else
	return CFG_DEFAULT_LOG_FILE;
#endif
}

// -----------------------------------------------------------------------
const char * cfg_fgetstr(em400_cfg *cfg, const char *key_format, ...)
{
	char key[MAX_KEY_LEN];
	va_list vl;
	va_start(vl, key_format);
	vsnprintf(key, MAX_KEY_LEN, key_format, vl);
	va_end(vl);
	return cfg_getstr(cfg, key, NULL);
}

// -----------------------------------------------------------------------
int cfg_fgetint(em400_cfg *cfg, const char *key_format, ...)
{
	char key[MAX_KEY_LEN];
	va_list vl;
	va_start(vl, key_format);
	vsnprintf(key, MAX_KEY_LEN, key_format, vl);
	va_end(vl);
	return cfg_getint(cfg, key, -1);
}

// -----------------------------------------------------------------------
double cfg_fgetdouble(em400_cfg *cfg, const char *key_format, ...)
{
	char key[MAX_KEY_LEN];
	va_list vl;
	va_start(vl, key_format);
	vsnprintf(key, MAX_KEY_LEN, key_format, vl);
	va_end(vl);
	return cfg_getdouble(cfg, key, -1);
}

// -----------------------------------------------------------------------
int cfg_fgetbool(em400_cfg *cfg, const char *key_format, ...)
{
	char key[MAX_KEY_LEN];
	va_list vl;
	va_start(vl, key_format);
	vsnprintf(key, MAX_KEY_LEN, key_format, vl);
	va_end(vl);
	return cfg_getbool(cfg, key, -1);
}

// -----------------------------------------------------------------------
int cfg_fgetint_def(em400_cfg *cfg, int def, const char *key_format, ...)
{
	char key[MAX_KEY_LEN];
	va_list vl;
	va_start(vl, key_format);
	vsnprintf(key, MAX_KEY_LEN, key_format, vl);
	va_end(vl);
	return cfg_getint(cfg, key, def);
}

// -----------------------------------------------------------------------
int cfg_fgetbool_def(em400_cfg *cfg, int def, const char *key_format, ...)
{
	char key[MAX_KEY_LEN];
	va_list vl;
	va_start(vl, key_format);
	vsnprintf(key, MAX_KEY_LEN, key_format, vl);
	va_end(vl);
	return cfg_getbool(cfg, key, def);
}

// -----------------------------------------------------------------------
int cfg_fcontains(em400_cfg *cfg, const char *key_format, ...)
{
	char key[MAX_KEY_LEN];
	va_list vl;
	va_start(vl, key_format);
	vsnprintf(key, MAX_KEY_LEN, key_format, vl);
	va_end(vl);
	return cfg_contains(cfg, key);
}
