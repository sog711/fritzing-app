/*******************************************************************

Part of the Fritzing project - http://fritzing.org
Copyright (c) 2025, 2026 Fritzing GmbH

Fritzing is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Fritzing is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Fritzing.  If not, see <http://www.gnu.org/licenses/>.

********************************************************************/

#include "svgcursorbuilder.h"
#include "../debugdialog.h"

#include <QApplication>
#include <QFile>
#include <QXmlStreamReader>
#include <QSvgRenderer>
#include <QPainter>
#include <QGuiApplication>
#include <QScreen>
#include <QRegularExpression>
#include <QKeyEvent>
#include <QEvent>
#include <QDebug>

// Static cursor pointers
QCursor * SvgCursorBuilder::BendpointCursor = nullptr;
QCursor * SvgCursorBuilder::NewBendpointCursor = nullptr;
QCursor * SvgCursorBuilder::MakeWireCursor = nullptr;
QCursor * SvgCursorBuilder::MakeCurveCursor = nullptr;
QCursor * SvgCursorBuilder::RubberbandCursor = nullptr;
QCursor * SvgCursorBuilder::RotateCursor = nullptr;
QCursor * SvgCursorBuilder::SpotFaceConnectCursor = nullptr;
QCursor * SvgCursorBuilder::SpotFaceCutterCursor = nullptr;

SvgCursorBuilder SvgCursorBuilder::TheSvgCursorBuilder;
static QList<QObject *> Listeners;
static QList<QCursor **> Cursors;

SvgCursorBuilder::SvgCursorBuilder() : QObject()
{
	m_blocked = false;
}

SvgCursorBuilder * SvgCursorBuilder::instance()
{
	return &TheSvgCursorBuilder;
}

void SvgCursorBuilder::cleanup() {
	for (QCursor ** cursor : Cursors) {
		delete *cursor;
	}
	Cursors.clear();

	// Clean up PNG cursors loaded separately
	delete RubberbandCursor;
	RubberbandCursor = nullptr;
}

void SvgCursorBuilder::initCursors()
{
	if (BendpointCursor == nullptr) {
		QStringList names;

		// All cursors are now loaded from SVG files
		names << ":resources/images/cursor/bendpoint.svg"
			  << ":resources/images/cursor/new_bendpoint.svg"
			  << ":resources/images/cursor/make_wire.svg"
			  << ":resources/images/cursor/curve.svg"
			  << ":resources/images/cursor/rotate.svg"
			  << ":resources/images/cursor/spot_face_connect.svg"
			  << ":resources/images/cursor/spot_face_cutter.svg";

		Cursors << &BendpointCursor
		        << &NewBendpointCursor
		        << &MakeWireCursor
		        << &MakeCurveCursor
		        << &RotateCursor
		        << &SpotFaceConnectCursor
		        << &SpotFaceCutterCursor;

		for (int i = 0; i < Cursors.count(); i++) {
			*Cursors.at(i) = new QCursor(createCursor(names.at(i)));
		}

		// Load rubberband cursor from PNG (temporary until we have SVG version)
		QPixmap rubberbandPixmap(":resources/images/cursor/rubberband_move.png");
		RubberbandCursor = new QCursor(rubberbandPixmap, 0, 0);

		QApplication::instance()->installEventFilter(instance());
	}
}

CursorInfo SvgCursorBuilder::loadCursorFromSvg(const QString& svgPath, int requestedSize, qreal scale) {
	CursorInfo info;

	// Open the SVG file
	QFile file(svgPath);
	if (!file.open(QIODevice::ReadOnly)) {
		info.error = QString("Cannot open file: %1").arg(svgPath);
		return info;
	}

	// Parse the SVG XML to extract attributes
	QXmlStreamReader xml(&file);

	int svgWidth = 0;
	int svgHeight = 0;
	bool hasHotspotX = false;
	bool hasHotspotY = false;

	while (!xml.atEnd()) {
		xml.readNext();

		if (xml.isStartElement() && xml.name() == u"svg") {
			QXmlStreamAttributes attrs = xml.attributes();

			// Extract width attribute
			if (attrs.hasAttribute("width")) {
				QString widthStr = attrs.value("width").toString();
				// Remove units (px, pt, etc.) - just keep the number
				widthStr.remove(QRegularExpression("[^0-9.]"));
				svgWidth = widthStr.toInt();
			}

			// Extract height attribute
			if (attrs.hasAttribute("height")) {
				QString heightStr = attrs.value("height").toString();
				// Remove units (px, pt, etc.) - just keep the number
				heightStr.remove(QRegularExpression("[^0-9.]"));
				svgHeight = heightStr.toInt();
			}

			// Extract fritzing namespace attributes
			for (const QXmlStreamAttribute& attr : attrs) {
				if (attr.namespaceUri() == QLatin1String(FRITZING_SVG_NS)) {
					if (attr.name() == u"hotspot-x") {
						info.hotspotX = attr.value().toInt();
						hasHotspotX = true;
					} else if (attr.name() == u"hotspot-y") {
						info.hotspotY = attr.value().toInt();
						hasHotspotY = true;
					}
				}
			}
			break; // We only need the root <svg> element
		}
	}

	file.close();

	// Check for XML parsing errors
	if (xml.hasError()) {
		info.error = QString("XML parsing error: %1").arg(xml.errorString());
		return info;
	}

	// Validate required attributes
	if (svgWidth == 0 || svgHeight == 0) {
		info.error = "SVG must have width and height attributes (with numeric values)";
		return info;
	}

	if (!hasHotspotX || !hasHotspotY) {
		info.error = QString("SVG must have %1:hotspot-x and %1:hotspot-y attributes")
			.arg("fritzing");
		return info;
	}

	// Determine the actual size to render
	int baseSize = (requestedSize > 0) ? requestedSize : svgWidth;

	// Scale hotspot proportionally if the requested size differs from SVG width
	if (svgWidth != baseSize) {
		qreal scaleFactor = static_cast<qreal>(baseSize) / svgWidth;
		info.hotspotX = static_cast<int>(info.hotspotX * scaleFactor);
		info.hotspotY = static_cast<int>(info.hotspotY * scaleFactor);
	}

	// Apply HiDPI scaling to hotspot
	info.hotspotX = static_cast<int>(info.hotspotX * scale);
	info.hotspotY = static_cast<int>(info.hotspotY * scale);

	// Render the SVG to a pixmap
	QSvgRenderer renderer(svgPath);
	if (!renderer.isValid()) {
		info.error = "Failed to load SVG for rendering";
		return info;
	}

	int scaledSize = static_cast<int>(baseSize * scale);
	QPixmap pixmap(scaledSize, scaledSize);
	pixmap.fill(Qt::transparent);

	QPainter painter(&pixmap);
	renderer.render(&painter);
	painter.end();

	// Set the device pixel ratio for HiDPI support
	pixmap.setDevicePixelRatio(scale);

	info.pixmap = pixmap;
	info.valid = true;

	return info;
}

QCursor SvgCursorBuilder::createCursor(const QString& svgPath, int baseSize) {
	// Get the primary screen's device pixel ratio for HiDPI support
	qreal scale = qApp->primaryScreen()->devicePixelRatio();

	CursorInfo info = loadCursorFromSvg(svgPath, baseSize, scale);

	if (!info.valid) {
		qWarning() << "Failed to load cursor from" << svgPath << ":" << info.error;
		return QCursor(Qt::ArrowCursor);
	}

	return QCursor(info.pixmap, info.hotspotX, info.hotspotY);
}

void SvgCursorBuilder::addCursor(QObject * object, const QCursor & cursor)
{
	if (m_blocked) return;

	if (object == nullptr) return;

	if (Listeners.contains(object)) {
		if (Listeners.first() != object) {
			Listeners.removeOne(object);
			Listeners.push_front(object);
		}
		QApplication::changeOverrideCursor(cursor);
		return;
	}

	Listeners.push_front(object);
	connect(object, SIGNAL(destroyed(QObject *)), this, SLOT(deleteCursor(QObject *)));
	QApplication::setOverrideCursor(cursor);
}

void SvgCursorBuilder::removeCursor(QObject * object)
{
	if (object == nullptr) return;

	if (Listeners.contains(object)) {
		disconnect(object, SIGNAL(destroyed(QObject *)), this, SLOT(deleteCursor(QObject *)));
		Listeners.removeOne(object);
		QApplication::restoreOverrideCursor();
	}
}

void SvgCursorBuilder::deleteCursor(QObject * object)
{
	removeCursor(object);
}

bool SvgCursorBuilder::eventFilter(QObject * object, QEvent * event)
{
	Q_UNUSED(object);

	switch (event->type()) {
	case QEvent::KeyPress:
	case QEvent::KeyRelease:
	{
		auto *keyEvent = static_cast<QKeyEvent*>(event);
		for (QObject * listener : Listeners) {
			if (listener != nullptr) {
				dynamic_cast<CursorKeyListener *>(listener)->cursorKeyEvent(keyEvent->modifiers());
				break;
			}
		}
	}
	break;
	default:
		break;
	}

	return false;
}

void SvgCursorBuilder::block() {
	m_blocked = true;
}

void SvgCursorBuilder::unblock() {
	m_blocked = false;
}
