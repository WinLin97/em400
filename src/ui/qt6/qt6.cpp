//  Copyright (c) 2022-2026 Jakub Filipowicz <jakubf@gmail.com>
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

#include <QApplication>
#include <QSettings>
#include <QTranslator>
#include <QLocale>
#include <QMessageBox>

#include "mainwindow.h"
#include "theme.h"

#include "ui/ui.h"
#include "libem400.h"

struct ui_qt6_data {
	int argc;
	char *argv[1];
	QApplication *app;
	const char *program;
};

static const char *ORG_NAME = "em400";
static const char *APP_NAME = "em400-qt";

// -----------------------------------------------------------------------
void * ui_qt6_setup(const char *call_name)
{
	struct ui_qt6_data *ui = (struct ui_qt6_data *) calloc(1, sizeof(struct ui_qt6_data));
	if (!ui) {
		return NULL;
	}

	ui->argc = 1;
	ui->argv[0] = (char*)"em400";
	ui->app = new QApplication(ui->argc, ui->argv);
	QApplication::setOrganizationName(ORG_NAME);
	QApplication::setApplicationName(APP_NAME);

	QTranslator *translator = new QTranslator(ui->app);
	if (translator->load(QLocale::system(), "em400-qt", "_", ":/i18n")) {
		ui->app->installTranslator(translator);
	}

	// default custom dark theme
	QSettings settings;
	em400_apply_theme(settings.value("ui/panelTheme", true).toBool());

	return ui;
}

// -----------------------------------------------------------------------
static int ui_qt6_poweron(void *data, const char *program)
{
	struct ui_qt6_data *ui = (struct ui_qt6_data *) data;
	ui->program = program;
	return E_OK;
}

// -----------------------------------------------------------------------
void ui_qt6_loop(void *data)
{
	struct ui_qt6_data *ui = (struct ui_qt6_data *) data;

	MainWindow w;
	w.show();

	QSettings settings;
	if (ui->program || settings.value("ui/startPoweredOn", false).toBool()) {
		w.startup_power_on(ui->program);
	}

	ui->app->exec();
}

// -----------------------------------------------------------------------
static void ui_qt6_msg(void *data, em400_sev_t sev, const char *text)
{
	(void) data;

	QMessageBox::Icon icon;
	QString title;
	switch (sev) {
		case EM400_MSG_ERROR:
			icon = QMessageBox::Critical;
			title = QCoreApplication::translate("ui_qt6", "Error");
			break;
		case EM400_MSG_WARNING:
			icon = QMessageBox::Warning;
			title = QCoreApplication::translate("ui_qt6", "Warning");
			break;
		default:
			icon = QMessageBox::Information;
			title = QCoreApplication::translate("ui_qt6", "Information");
			break;
	}

	QMessageBox box(icon, title, QString::fromUtf8(text), QMessageBox::Ok, nullptr);
	box.exec();
}

// -----------------------------------------------------------------------
static void ui_qt6_poweroff(void *data)
{
	em400_shutdown();
}

// -----------------------------------------------------------------------
void ui_qt6_destroy(void *data)
{
	struct ui_qt6_data *ui = (struct ui_qt6_data *) data;
	delete ui->app;
	free(ui);
}

// -----------------------------------------------------------------------
struct ui_drv ui_qt6 = {
	.name = "qt",
	.setup = ui_qt6_setup,
	.poweron = ui_qt6_poweron,
	.loop = ui_qt6_loop,
	.poweroff = ui_qt6_poweroff,
	.destroy = ui_qt6_destroy,
	.msg = ui_qt6_msg,
};

// vim: tabstop=4 shiftwidth=4 autoindent
