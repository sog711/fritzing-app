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

#include "FProbeMigrationDialog.h"
#include "mainwindow/mainwindow.h"
#include "debugdialog.h"

#include <QDialog>
#include <QRadioButton>
#include <QPushButton>
#include <QAbstractButton>
#include <QLabel>
#include <QJsonObject>
#include <QJsonDocument>

FProbeMigrationDialog::FProbeMigrationDialog(MainWindow *mainWindow)
	: QObject(mainWindow)
	, FProbe("MigrationDialog")
	, m_mainWindow(mainWindow)
{
	// Probes are called from the FTesting server thread; hop to the GUI thread for widget access.
	connect(this, &FProbeMigrationDialog::requestInfo, mainWindow, [this]() {
		QJsonObject result;
		QDialog *dialog = m_mainWindow->findChild<QDialog *>("partMigrationDialog");
		bool open = (dialog != nullptr && dialog->isVisible());
		result["open"] = open;
		if (open) {
			auto labelText = [dialog](const char *name) -> QString {
				QLabel *l = dialog->findChild<QLabel *>(name);
				return l ? l->text() : QString();
			};
			QRadioButton *oldRadio = dialog->findChild<QRadioButton *>("migrationOldRadio");
			QRadioButton *newRadio = dialog->findChild<QRadioButton *>("migrationNewRadio");
			QPushButton *silence = dialog->findChild<QPushButton *>("migrationSilenceButton");
			QPushButton *updateAll = dialog->findChild<QPushButton *>("migrationUpdateAllButton");
			QPushButton *prev = dialog->findChild<QPushButton *>("migrationPrevButton");
			QPushButton *next = dialog->findChild<QPushButton *>("migrationNextButton");

			result["instanceTitle"] = dialog->property("currentInstanceTitle").toString();
			result["selected"] = QString((newRadio && newRadio->isChecked()) ? "new" : "old");
			result["oldText"] = oldRadio ? oldRadio->text() : QString();
			result["newText"] = newRadio ? newRadio->text() : QString();
			result["counter"] = labelText("migrationCounterLabel");
			result["partTitle"] = labelText("migrationTitleLabel");
			result["reason"] = labelText("migrationReasonLabel");
			result["history"] = labelText("migrationHistoryLabel");
			result["silenceVisible"] = (silence != nullptr && silence->isVisible());
			result["updateAllVisible"] = (updateAll != nullptr && updateAll->isVisible());
			result["prevEnabled"] = (prev != nullptr && prev->isEnabled());
			result["nextEnabled"] = (next != nullptr && next->isEnabled());
		}
		m_lastResult = QVariant(QString(QJsonDocument(result).toJson(QJsonDocument::Compact)));
	}, Qt::BlockingQueuedConnection);

	connect(this, &FProbeMigrationDialog::requestAction, mainWindow, [this](const QString &action) {
		QJsonObject result;
		QDialog *dialog = m_mainWindow->findChild<QDialog *>("partMigrationDialog");
		if (dialog == nullptr) {
			result["ok"] = false;
			result["reason"] = "no migration dialog is open";
		}
		else {
			bool ok = true;
			QString reason;
			auto clickButton = [dialog, &ok, &reason](const char *name) {
				if (QAbstractButton *b = dialog->findChild<QAbstractButton *>(name)) b->click();
				else { ok = false; reason = QString("control '%1' not found").arg(name); }
			};
			if (action == "new") clickButton("migrationNewRadio");
			else if (action == "old") clickButton("migrationOldRadio");
			else if (action == "silence") clickButton("migrationSilenceButton");
			else if (action == "updateAll") clickButton("migrationUpdateAllButton");
			else if (action == "next") clickButton("migrationNextButton");
			else if (action == "previous") clickButton("migrationPrevButton");
			else if (action == "close") dialog->close();
			else { ok = false; reason = QString("unknown action '%1'").arg(action); }
			result["ok"] = ok;
			if (!ok) {
				result["reason"] = reason;
				DebugDialog::debug(QString("MigrationDialog probe action failed: %1").arg(reason));
			}
		}
		m_lastResult = QVariant(QString(QJsonDocument(result).toJson(QJsonDocument::Compact)));
	}, Qt::BlockingQueuedConnection);
}

QVariant FProbeMigrationDialog::read()
{
	Q_EMIT requestInfo();
	return m_lastResult;
}

void FProbeMigrationDialog::write(QVariant var)
{
	Q_EMIT requestAction(var.toString());
}
