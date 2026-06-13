/*******************************************************************

Part of the Fritzing project - http://fritzing.org
Copyright (c) 2026 Fritzing GmbH

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

#include "FProbeSelectedPartLabel.h"
#include "mainwindow/mainwindow.h"
#include "sketch/sketchwidget.h"
#include "items/itembase.h"
#include "items/partlabel.h"
#include "debugdialog.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QGraphicsScene>
#include <QSet>

#include <cmath>

namespace {

struct LabelAction {
	const char *name;
	double degrees;
	Qt::Orientations orientation;
	bool pcbOnly;
};

// Mirrors the entries of the label's Flip/Rotate context menu, including
// the 45-degree variants only being available in PCB view (see
// PartLabel::initMenu and PartLabel::rotateFlip).
constexpr LabelAction labelActions[] = {
	{"Rotate 45° Clockwise", 45, {}, true},
	{"Rotate 90° Clockwise", 90, {}, false},
	{"Rotate 135° Clockwise", 135, {}, true},
	{"Rotate 180°", 180, {}, false},
	{"Rotate 135° Counter Clockwise", 225, {}, true},
	{"Rotate 90° Counter Clockwise", 270, {}, false},
	{"Rotate 45° Counter Clockwise", 315, {}, true},
	{"Flip Horizontal", 0, Qt::Horizontal, false},
	{"Flip Vertical", 0, Qt::Vertical, false},
};

}

FProbeSelectedPartLabel::FProbeSelectedPartLabel(MainWindow *mainWindow)
	: QObject(mainWindow)
	, FProbe("SelectedPartLabel")
	, m_mainWindow(mainWindow)
{
	// Probes are called from the FTesting server thread; hop to the GUI
	// thread for all scene access.
	connect(this, &FProbeSelectedPartLabel::requestInfo, mainWindow, [this]() {
		QJsonObject result;
		SketchWidget *view = nullptr;
		PartLabel *partLabel = findSelectedPartLabel(view, result);
		if (partLabel) {
			ItemBase *owner = partLabel->owner();
			result["found"] = true;
			result["text"] = owner->instanceTitle();
			result["visible"] = partLabel->isVisible();

			QPointF pos = partLabel->pos();
			QJsonObject position;
			position["x"] = pos.x();
			position["y"] = pos.y();
			result["position"] = position;

			QTransform t = partLabel->transform();
			double rotation = std::atan2(t.m12(), t.m11()) * 180.0 / M_PI;
			if (rotation < 0) rotation += 360.0;
			result["rotation"] = rotation;
			result["flipped"] = (t.m11() * t.m22() - t.m12() * t.m21()) < 0;

			QRectF bounds = partLabel->sceneBoundingRect();
			QJsonObject boundsObj;
			boundsObj["x"] = bounds.x();
			boundsObj["y"] = bounds.y();
			boundsObj["width"] = bounds.width();
			boundsObj["height"] = bounds.height();
			result["bounds"] = boundsObj;

			bool isPCB = view->viewID() == ViewLayer::PCBView;
			QJsonArray actions;
			for (const LabelAction &action : labelActions) {
				if (action.pcbOnly && !isPCB) continue;
				actions.append(QString::fromUtf8(action.name));
			}
			result["actions"] = actions;
		}
		m_lastResult = QVariant(QString(QJsonDocument(result).toJson(QJsonDocument::Compact)));
	}, Qt::BlockingQueuedConnection);

	connect(this, &FProbeSelectedPartLabel::requestAction, mainWindow, [this](const QString &actionName) {
		QJsonObject result;
		SketchWidget *view = nullptr;
		PartLabel *partLabel = findSelectedPartLabel(view, result);
		if (partLabel) {
			bool isPCB = view->viewID() == ViewLayer::PCBView;
			bool done = false;
			for (const LabelAction &action : labelActions) {
				if (actionName != QString::fromUtf8(action.name)) continue;
				if (action.pcbOnly && !isPCB) {
					result["found"] = false;
					result["reason"] = QString("action '%1' is not available in this view").arg(actionName);
				}
				else {
					// Same code path as the label's context menu, see
					// PartLabel::rotateFlip; goes through the undo stack.
					partLabel->owner()->rotateFlipPartLabel(action.degrees, action.orientation);
					result["found"] = true;
				}
				done = true;
				break;
			}
			if (!done) {
				result["found"] = false;
				result["reason"] = QString("unknown action '%1'").arg(actionName);
			}
		}
		if (!result["found"].toBool()) {
			DebugDialog::debug(QString("SelectedPartLabel write failed: %1").arg(result["reason"].toString()));
		}
		m_lastResult = QVariant(QString(QJsonDocument(result).toJson(QJsonDocument::Compact)));
	}, Qt::BlockingQueuedConnection);
}

QVariant FProbeSelectedPartLabel::read()
{
	Q_EMIT requestInfo();
	return m_lastResult;
}

void FProbeSelectedPartLabel::write(QVariant var)
{
	Q_EMIT requestAction(var.toString());
}

PartLabel * FProbeSelectedPartLabel::findSelectedPartLabel(SketchWidget * & view, QJsonObject &result)
{
	view = m_mainWindow->currentGraphicsView();
	if (!view) {
		result["found"] = false;
		result["reason"] = "no current view";
		return nullptr;
	}
	// The untranslated name, "breadboard", "schematic" or "pcb"
	result["view"] = ViewLayer::viewIDNaturalName(view->viewID());

	// A part is split into one ItemBase per layer in PCB view, all of which
	// report as selected together; collapse them onto the layer kin chief so
	// a single selected part counts once.
	QSet<ItemBase *> selected;
	for (QGraphicsItem *item : view->scene()->selectedItems()) {
		auto *base = dynamic_cast<ItemBase *>(item);
		if (base) selected.insert(base->layerKinChief());
	}
	if (selected.isEmpty()) {
		result["found"] = false;
		result["reason"] = "no part selected";
		return nullptr;
	}
	if (selected.count() > 1) {
		result["found"] = false;
		result["reason"] = QString("%1 parts selected, expected 1").arg(selected.count());
		return nullptr;
	}

	ItemBase *part = *selected.constBegin();
	PartLabel *partLabel = part->partLabel();
	if (!partLabel || !partLabel->initialized()) {
		result["found"] = false;
		result["reason"] = QString("part '%1' has no initialized label").arg(part->instanceTitle());
		return nullptr;
	}
	return partLabel;
}
