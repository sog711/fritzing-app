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

#include "FProbePart.h"
#include "mainwindow/mainwindow.h"
#include "sketch/sketchwidget.h"
#include "items/itembase.h"
#include "commands.h"
#include "waitpushundostack.h"
#include "debugdialog.h"

#include <QJsonDocument>
#include <QGraphicsScene>

FProbePart::FProbePart(MainWindow *mainWindow)
	: QObject(mainWindow)
	, FProbe("PartProbe")
	, m_mainWindow(mainWindow)
{
	connect(this, &FProbePart::requestOperation, mainWindow, [this](const QString &jsonCommand) {
		QJsonDocument doc = QJsonDocument::fromJson(jsonCommand.toUtf8());
		QJsonObject params = doc.object();
		QString cmd = params["cmd"].toString();

		SketchWidget *view = currentView();
		if (!view) {
			m_lastResult = QVariant(QString("{\"ok\":false,\"error\":\"no active view\"}"));
			return;
		}

		QJsonObject result;

		if (cmd == "getPosition") {
			result = handleGetPosition(view, params);
		} else if (cmd == "movePart") {
			result = handleMovePart(view, params);
		} else if (cmd == "movePartRelative") {
			result = handleMovePartRelative(view, params);
		} else {
			result["ok"] = false;
			result["error"] = QString("unknown command '%1'").arg(cmd);
		}

		QJsonDocument resultDoc(result);
		m_lastResult = QVariant(QString(resultDoc.toJson(QJsonDocument::Compact)));
	}, Qt::BlockingQueuedConnection);
}

QVariant FProbePart::read()
{
	return m_lastResult;
}

void FProbePart::write(QVariant var)
{
	Q_EMIT requestOperation(var.toString());
}

SketchWidget * FProbePart::currentView()
{
	return m_mainWindow->currentGraphicsView();
}

ItemBase * FProbePart::findPartByTitle(SketchWidget *view, const QString &title)
{
	for (QGraphicsItem *item : view->scene()->items()) {
		auto *base = dynamic_cast<ItemBase *>(item);
		if (!base) continue;
		if (base->instanceTitle() != title) continue;
		if (base->viewID() != view->viewID()) continue;
		return base;
	}
	return nullptr;
}

QJsonObject FProbePart::handleGetPosition(SketchWidget *view, const QJsonObject &params)
{
	QJsonObject result;
	QString partTitle = params["part"].toString();
	bool select = params["select"].toBool();

	ItemBase *part = findPartByTitle(view, partTitle);
	if (!part) {
		result["ok"] = false;
		result["error"] = QString("part '%1' not found").arg(partTitle);
		return result;
	}

	if (select) {
		view->scene()->clearSelection();
		part->setSelected(true);
	}

	QPointF pos = part->pos();
	result["ok"] = true;
	result["x"] = pos.x();
	result["y"] = pos.y();
	return result;
}

QJsonObject FProbePart::handleMovePart(SketchWidget *view, const QJsonObject &params)
{
	QJsonObject result;
	QString partTitle = params["part"].toString();

	ItemBase *part = findPartByTitle(view, partTitle);
	if (!part) {
		result["ok"] = false;
		result["error"] = QString("part '%1' not found").arg(partTitle);
		return result;
	}

	QPointF oldPos = part->pos();
	QPointF newPos(params["x"].toDouble(), params["y"].toDouble());

	auto *parentCommand = new QUndoCommand(QObject::tr("Move part"));
	new SimpleMoveItemCommand(view, part->id(), oldPos, newPos, parentCommand);
	view->undoStack()->push(parentCommand);

	result["ok"] = true;
	return result;
}

QJsonObject FProbePart::handleMovePartRelative(SketchWidget *view, const QJsonObject &params)
{
	QJsonObject result;
	QString partTitle = params["part"].toString();

	ItemBase *part = findPartByTitle(view, partTitle);
	if (!part) {
		result["ok"] = false;
		result["error"] = QString("part '%1' not found").arg(partTitle);
		return result;
	}

	QPointF oldPos = part->pos();
	double dx = params["dx"].toDouble();
	double dy = params["dy"].toDouble();
	QPointF newPos = oldPos + QPointF(dx, dy);

	auto *parentCommand = new QUndoCommand(QObject::tr("Move part"));
	new SimpleMoveItemCommand(view, part->id(), oldPos, newPos, parentCommand);
	view->undoStack()->push(parentCommand);

	result["ok"] = true;
	return result;
}
