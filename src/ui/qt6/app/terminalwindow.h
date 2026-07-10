//  Copyright (c) 2026 Jakub Filipowicz <jakubf@gmail.com>
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

#ifndef TERMINALWINDOW_H
#define TERMINALWINDOW_H

#include <QMainWindow>
#include <QWidget>
#include <QFont>
#include <QString>

class QTcpSocket;
class QTimer;
class QAction;

// A fixed 80x24 character grid over a raw TCP byte pipe to a MERA-400 terminal
// device (127.0.0.1:<port>) - the same wire emterm speaks. No ANSI/VT escapes;
// only CR/LF/BS/HT/BEL/FF are interpreted on the incoming stream.
class TerminalScreen : public QWidget
{
	Q_OBJECT

public:
	static const int ROWS = 24;
	static const int COLS = 80;

	explicit TerminalScreen(int port, QWidget *parent = nullptr);

	void reconnect();
	void clear();
	void set_local_echo(bool on);
	bool local_echo() const { return echo; }
	void set_terminal_font(const QFont &f);
	bool is_connected() const;

	QSize sizeHint() const override;

signals:
	void connection_changed(bool connected);

protected:
	void keyPressEvent(QKeyEvent *ev) override;
	void paintEvent(QPaintEvent *ev) override;

private:
	int port;
	QTcpSocket *sock;
	QFont fnt;
	int cell_w = 0, cell_h = 0, ascent = 0;

	char cells[ROWS][COLS];
	int cx = 0, cy = 0;
	bool echo;

	// bytes arrive ~1ms apart (baud emulation), each in its own readyRead, so
	// update()'s same-pass coalescing never triggers; this timer caps repaints
	// to display rate instead of repainting per byte.
	QTimer *repaint_timer = nullptr;

	void recompute_metrics();
	void schedule_repaint();
	void put_byte(unsigned char c);
	void line_feed();
	void scroll_up();

private slots:
	void slot_ready_read();
};

// -----------------------------------------------------------------------
class TerminalWindow : public QMainWindow
{
	Q_OBJECT

public:
	explicit TerminalWindow(int port, const QString &label, QWidget *parent = nullptr);

	void apply_terminal_font();

private:
	int port;
	QString label;
	TerminalScreen *screen;
	QAction *echo_action;

	void update_title(bool connected);
	void choose_font();
};

#endif // TERMINALWINDOW_H

// vim: tabstop=4 shiftwidth=4 autoindent
