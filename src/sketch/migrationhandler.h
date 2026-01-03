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

#ifndef MIGRATIONHANDLER_H
#define MIGRATIONHANDLER_H

#include <QObject>
#include <QList>
#include <QString>
#include <QDate>

#include "../model/modelpartshared.h"

class ItemBase;
class ModelPart;
class SketchWidget;
class MainWindow;
class QDialog;
class QLabel;
class QPushButton;

struct MigrationInfo {
	qint64 itemId;          // current item ID (changes after swap)
	qint64 originalItemId;  // original item ID (for restoring after undo)
	QString oldModuleID;    // module ID to swap back to old
	QString newModuleID;    // module ID to swap to new
	QString instanceTitle;  // for display purposes
	QList<HistoryEntry> relevantHistory;
	QString effectiveMode;  // "silent", "ask", or "forced"
	bool isSwapped;         // currently showing new part?
};

class MigrationHandler : public QObject
{
	Q_OBJECT

public:
	explicit MigrationHandler(SketchWidget* sketchWidget, QObject* parent = nullptr);

	// Main interface
	void queueMigration(ItemBase* itemBase, ModelPart* oldPart,
	                    ModelPart* newPart, const QList<HistoryEntry>& history);
	void processMigrations();
	bool hasPendingMigrations() const;
	void clearPendingMigrations();

	// Get relevant history entries based on date filtering
	static QList<HistoryEntry> getRelevantHistory(ModelPart* instancePart,
	                                              const QList<HistoryEntry>& allHistory);
	static QString computeEffectiveMode(const QList<HistoryEntry>& history);

	// Get current item for a migration (may change after swaps)
	ItemBase* currentItem(int index) const;
	ItemBase* currentItem() const;  // for current migration

private Q_SLOTS:
	void handlePendingMigrationDialog();

private:
	void createMigrationDialog();
	void updateDialogForCurrentMigration();
	void centerAndZoomOnItem(ItemBase* item);

	void swapCurrentPart();
	void confirmCurrentMigration();
	void skipCurrentMigration();
	void silenceCurrentMigration();
	void processNextMigration();
	void closeDialog();

	MainWindow* findMainWindow() const;

	SketchWidget* m_sketchWidget;
	QList<MigrationInfo> m_pendingMigrations;
	int m_currentIndex;

	// Dialog widgets
	QDialog* m_dialog;
	QLabel* m_counterLabel;
	QLabel* m_titleLabel;
	QLabel* m_historyLabel;
	QPushButton* m_swapButton;
	QPushButton* m_confirmButton;
	QPushButton* m_skipButton;
	QPushButton* m_silenceButton;
};

#endif // MIGRATIONHANDLER_H
