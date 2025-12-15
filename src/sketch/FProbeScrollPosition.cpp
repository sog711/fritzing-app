/*******************************************************************

Part of the Fritzing project - http://fritzing.org
Copyright (c) 2025 Fritzing GmbH

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

#include "FProbeScrollPosition.h"

#include "../debugdialog.h"

#include <QScrollBar>
#include <QJsonDocument>
#include <QJsonObject>

FProbeScrollPosition::FProbeScrollPosition(SketchWidget * sketchWidget, const QString& viewSuffix) :
	FProbe(viewSuffix.isEmpty() ? "ScrollPosition" : ("ScrollPosition_" + viewSuffix).toStdString()),
	m_sketchWidget(sketchWidget) {
	QString probeName = viewSuffix.isEmpty() ? "ScrollPosition" : ("ScrollPosition_" + viewSuffix);
	DebugDialog::debug(QString("FProbeScrollPosition registered: %1").arg(probeName));
}

FProbeScrollPosition::~FProbeScrollPosition() {
}

QVariant FProbeScrollPosition::read() {
	if (!m_sketchWidget) {
		DebugDialog::debug("ScrollPosition: No sketch widget");
		return QVariant();
	}

	QJsonObject result;

	// Get scrollbar values
	QScrollBar* hBar = m_sketchWidget->horizontalScrollBar();
	QScrollBar* vBar = m_sketchWidget->verticalScrollBar();

	if (hBar) {
		result["hValue"] = hBar->value();
		result["hMin"] = hBar->minimum();
		result["hMax"] = hBar->maximum();
		result["hPageStep"] = hBar->pageStep();
		result["hVisible"] = hBar->isVisible();
	}

	if (vBar) {
		result["vValue"] = vBar->value();
		result["vMin"] = vBar->minimum();
		result["vMax"] = vBar->maximum();
		result["vPageStep"] = vBar->pageStep();
		result["vVisible"] = vBar->isVisible();
	}

	// Get zoom level
	result["zoom"] = m_sketchWidget->currentZoom();

	// Get view type
	QString viewType;
	switch (m_sketchWidget->viewID()) {
	case ViewLayer::BreadboardView:
		viewType = "breadboard";
		break;
	case ViewLayer::SchematicView:
		viewType = "schematic";
		break;
	case ViewLayer::PCBView:
		viewType = "pcb";
		break;
	default:
		viewType = "unknown";
		break;
	}
	result["viewType"] = viewType;

	// Get scene rect
	if (m_sketchWidget->scene()) {
		QRectF sceneRect = m_sketchWidget->scene()->sceneRect();
		QJsonObject sceneRectObj;
		sceneRectObj["x"] = sceneRect.x();
		sceneRectObj["y"] = sceneRect.y();
		sceneRectObj["width"] = sceneRect.width();
		sceneRectObj["height"] = sceneRect.height();
		result["sceneRect"] = sceneRectObj;
	}

	// Get viewport rect in scene coordinates
	QRectF viewportRect = m_sketchWidget->mapToScene(m_sketchWidget->viewport()->rect()).boundingRect();
	QJsonObject viewportRectObj;
	viewportRectObj["x"] = viewportRect.x();
	viewportRectObj["y"] = viewportRect.y();
	viewportRectObj["width"] = viewportRect.width();
	viewportRectObj["height"] = viewportRect.height();
	result["viewportRect"] = viewportRectObj;

	// Get transform matrix diagonal (scale factors)
	QTransform transform = m_sketchWidget->transform();
	result["scaleX"] = transform.m11();
	result["scaleY"] = transform.m22();

	// Serialize to JSON string
	QJsonDocument doc(result);
	QString jsonString = QString(doc.toJson(QJsonDocument::Compact));

	DebugDialog::debug(QString("ScrollPosition: %1").arg(jsonString));

	return QVariant(jsonString);
}
