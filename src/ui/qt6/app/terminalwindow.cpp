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

#include <QTcpSocket>
#include <QHostAddress>
#include <QKeyEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QFontMetrics>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QFontDialog>
#include <QSettings>
#include <QVBoxLayout>
#include <QTimer>

#include "terminalwindow.h"
#include "theme.h"

static const QColor c_term_bg(0x10, 0x10, 0x10);
static const QColor c_term_fg(0xc8, 0xc8, 0xc8);

// -----------------------------------------------------------------------
TerminalScreen::TerminalScreen(int port, QWidget *parent) :
	QWidget(parent), port(port), echo(QSettings().value("ui/terminalLocalEcho", true).toBool())
{
	setFocusPolicy(Qt::StrongFocus);
	setAttribute(Qt::WA_OpaquePaintEvent);

	em400_apply_terminal_font(fnt);
	recompute_metrics();
	clear();

	repaint_timer = new QTimer(this);
	repaint_timer->setSingleShot(true);
	connect(repaint_timer, &QTimer::timeout, this, [this]() {
		update();
	});

	sock = new QTcpSocket(this);
	connect(sock, &QTcpSocket::readyRead, this, &TerminalScreen::slot_ready_read);
	connect(sock, &QTcpSocket::connected, this, [this]() {
		emit connection_changed(true);
	});
	connect(sock, &QTcpSocket::disconnected, this, [this]() {
		emit connection_changed(false);
	});
	reconnect();
}

// -----------------------------------------------------------------------
void TerminalScreen::recompute_metrics()
{
	QFontMetrics fm(fnt);
	cell_w = fm.horizontalAdvance(QChar('M'));
	cell_h = fm.height();
	ascent = fm.ascent();
}

// -----------------------------------------------------------------------
void TerminalScreen::set_terminal_font(const QFont &f)
{
	fnt = f;
	recompute_metrics();
	updateGeometry();
	update();
}

// -----------------------------------------------------------------------
QSize TerminalScreen::sizeHint() const
{
	return QSize(COLS * cell_w, ROWS * cell_h);
}

// -----------------------------------------------------------------------
bool TerminalScreen::is_connected() const
{
	return sock->state() == QAbstractSocket::ConnectedState;
}

// -----------------------------------------------------------------------
void TerminalScreen::reconnect()
{
	if (sock->state() != QAbstractSocket::UnconnectedState) {
		sock->abort();
	}
	sock->connectToHost(QHostAddress::LocalHost, port);
}

// -----------------------------------------------------------------------
void TerminalScreen::clear()
{
	for (int r=0 ; r<ROWS ; r++) {
		for (int c=0 ; c<COLS ; c++) {
			cells[r][c] = ' ';
		}
	}
	cx = 0;
	cy = 0;
	update();
}

// -----------------------------------------------------------------------
void TerminalScreen::set_local_echo(bool on)
{
	echo = on;
	QSettings().setValue("ui/terminalLocalEcho", on);
}

// -----------------------------------------------------------------------
void TerminalScreen::slot_ready_read()
{
	QByteArray data = sock->readAll();
	for (char b : data) {
		put_byte((unsigned char) b);
	}
	schedule_repaint();
}

// -----------------------------------------------------------------------
void TerminalScreen::schedule_repaint()
{
	if (!repaint_timer->isActive()) {
		repaint_timer->start(16);
	}
}

// -----------------------------------------------------------------------
void TerminalScreen::line_feed()
{
	cy++;
	if (cy >= ROWS) {
		scroll_up();
		cy = ROWS - 1;
	}
}

// -----------------------------------------------------------------------
void TerminalScreen::scroll_up()
{
	for (int r=0 ; r<ROWS-1 ; r++) {
		for (int c=0 ; c<COLS ; c++) {
			cells[r][c] = cells[r+1][c];
		}
	}
	for (int c=0 ; c<COLS ; c++) {
		cells[ROWS-1][c] = ' ';
	}
}

// -----------------------------------------------------------------------
void TerminalScreen::put_byte(unsigned char c)
{
	switch (c) {
		case '\r':
			cx = 0;
			break;
		case '\n':
			line_feed();
			break;
		case 0x08:
			if (cx > 0) cx--;
			break;
		case '\t':
			cx = (cx + 8) & ~7;
			if (cx >= COLS) cx = COLS - 1;
			break;
		case 0x07:
			// BEL: no bell, deliberately silent
			break;
		case 0x0c:
			clear();
			break;
		default:
			if ((c >= 0x20) && (c <= 0x7e)) {
				if (cx >= COLS) {
					cx = 0;
					line_feed();
				}
				cells[cy][cx] = (char) c;
				cx++;
			}
			break;
	}
}

// -----------------------------------------------------------------------
void TerminalScreen::keyPressEvent(QKeyEvent *ev)
{
	if (!is_connected()) {
		QWidget::keyPressEvent(ev);
		return;
	}

	QByteArray out;
	switch (ev->key()) {
		case Qt::Key_Return:
		case Qt::Key_Enter:
			out.append('\r');
			break;
		case Qt::Key_Backspace:
			out.append(char((ev->modifiers() & Qt::ControlModifier) ? 0x7f : 0x08));
			break;
		case Qt::Key_Tab:
			out.append('\t');
			break;
		case Qt::Key_Escape:
			out.append(char(0x1b));
			break;
		case Qt::Key_Delete:
			out.append(char(0x7f));
			break;
		default:
			out = ev->text().toLatin1();
			break;
	}

	if (out.isEmpty()) {
		QWidget::keyPressEvent(ev);
		return;
	}

	sock->write(out);
	if (echo) {
		for (char b : out) {
			put_byte((unsigned char) b);
		}
		update();
	}
}

// -----------------------------------------------------------------------
void TerminalScreen::paintEvent(QPaintEvent *ev)
{
	(void) ev;

	QPainter p(this);
	p.fillRect(rect(), c_term_bg);
	p.setFont(fnt);
	p.setPen(c_term_fg);

	for (int r=0 ; r<ROWS ; r++) {
		QString line = QString::fromLatin1(cells[r], COLS);
		p.drawText(0, r * cell_h + ascent, line);
	}

	if (is_connected() && hasFocus()) {
		int ccx = cx < COLS ? cx : COLS - 1;
		p.fillRect(ccx * cell_w, cy * cell_h, cell_w, cell_h, c_term_fg);
		p.setPen(c_term_bg);
		p.drawText(ccx * cell_w, cy * cell_h + ascent, QString(QChar(cells[cy][ccx])));
	}
}

// -----------------------------------------------------------------------
TerminalWindow::TerminalWindow(int port, const QString &label, QWidget *parent) :
	QMainWindow(parent), port(port), label(label)
{
	screen = new TerminalScreen(port, this);
	screen->setFixedSize(screen->sizeHint());

	QWidget *center = new QWidget(this);
	QVBoxLayout *box = new QVBoxLayout(center);
	box->setContentsMargins(0, 0, 0, 0);
	box->addWidget(screen, 0, Qt::AlignCenter);
	setCentralWidget(center);

	QMenu *menu = menuBar()->addMenu(tr("Terminal"));

	QAction *reconnect = menu->addAction(tr("Reconnect"));
	connect(reconnect, &QAction::triggered, screen, &TerminalScreen::reconnect);

	QAction *clear = menu->addAction(tr("Clear"));
	connect(clear, &QAction::triggered, screen, &TerminalScreen::clear);

	echo_action = menu->addAction(tr("Local echo"));
	echo_action->setCheckable(true);
	echo_action->setChecked(screen->local_echo());
	connect(echo_action, &QAction::toggled, screen, &TerminalScreen::set_local_echo);

	QAction *font = menu->addAction(tr("Font..."));
	connect(font, &QAction::triggered, this, &TerminalWindow::choose_font);

	menu->addSeparator();
	QAction *close = menu->addAction(tr("Close"));
	connect(close, &QAction::triggered, this, &QWidget::close);

	connect(screen, &TerminalScreen::connection_changed, this, &TerminalWindow::update_title);

	update_title(false);
}

// -----------------------------------------------------------------------
void TerminalWindow::update_title(bool connected)
{
	setWindowTitle(tr("Terminal %1 port %2 (%3)")
		.arg(label)
		.arg(port)
		.arg(connected ? tr("connected") : tr("disconnected")));
}

// -----------------------------------------------------------------------
void TerminalWindow::apply_terminal_font()
{
	QFont f;
	em400_apply_terminal_font(f);
	screen->set_terminal_font(f);
	screen->setFixedSize(screen->sizeHint());
}

// -----------------------------------------------------------------------
void TerminalWindow::choose_font()
{
	QFont initial;
	em400_apply_terminal_font(initial);
	bool ok = false;
	QFont chosen = QFontDialog::getFont(&ok, initial, this, tr("Terminal font"), QFontDialog::MonospacedFonts);
	if (!ok) return;

	QSettings s;
	s.setValue("ui/terminalFontFamily", chosen.family());
	s.setValue("ui/terminalFontSize", chosen.pointSize());
	emit signal_font_changed();
}

// vim: tabstop=4 shiftwidth=4 autoindent
