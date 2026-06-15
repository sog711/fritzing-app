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

#include "migrationhandler.h"
#include "sketchwidget.h"
#include "../items/itembase.h"
#include "../model/modelpart.h"
#include "../mainwindow/mainwindow.h"
#include "../debugdialog.h"
#include "../waitpushundostack.h"

#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QGraphicsScene>
#include <QFont>
#include <QScrollArea>

MigrationHandler::MigrationHandler(SketchWidget* sketchWidget, QObject* parent)
	: QObject(parent)
	, m_sketchWidget(sketchWidget)
	, m_currentIndex(0)
	, m_dialog(nullptr)
	, m_counterLabel(nullptr)
	, m_titleLabel(nullptr)
	, m_historyLabel(nullptr)
	, m_swapButton(nullptr)
	, m_confirmButton(nullptr)
	, m_skipButton(nullptr)
	, m_silenceButton(nullptr)
{
}

void MigrationHandler::queueMigration(ItemBase* itemBase, ModelPart* oldPart,
                                      ModelPart* newPart, const QList<HistoryEntry>& history)
{
	MigrationInfo info;
	info.itemId = itemBase->id();
	info.originalItemId = itemBase->id();
	info.oldModuleID = oldPart->moduleID();
	info.newModuleID = newPart->moduleID();
	info.instanceTitle = itemBase->instanceTitle();
	info.relevantHistory = history;
	info.effectiveMode = computeEffectiveMode(history);
	info.isSwapped = false;

	m_pendingMigrations.append(info);
}

ItemBase* MigrationHandler::currentItem(int index) const
{
	if (index < 0 || index >= m_pendingMigrations.size()) return nullptr;
	// The part may live in any view (it isn't necessarily present in PCB), so
	// resolve it across all views by its shared cross-view id.
	MainWindow* mainWindow = findMainWindow();
	if (mainWindow == nullptr) return nullptr;
	return mainWindow->findItemInAnyView(m_pendingMigrations[index].itemId);
}

ItemBase* MigrationHandler::currentItem() const
{
	return currentItem(m_currentIndex);
}

void MigrationHandler::processMigrations()
{
	if (m_pendingMigrations.isEmpty()) return;

	// Collect silent migrations to auto-apply now; keep the rest for the dialog.
	QList<MigrationInfo> nonSilentMigrations;
	QList<ItemBase*> silentItems;
	for (const MigrationInfo& info : m_pendingMigrations) {
		if (info.effectiveMode == "silent") {
			ItemBase* item = m_sketchWidget->findItem(info.itemId);
			if (item != nullptr) {
				DebugDialog::debug(QString("Auto-applying silent migration for %1")
				                       .arg(info.instanceTitle));
				silentItems.append(item);
			}
		} else {
			nonSilentMigrations.append(info);
		}
	}

	// Auto-apply all silent migrations as a single undoable command, with no dialog
	// and no feedback popup. swapObsolete() swaps each item to its replacedby target
	// and ports the special-cased properties (resistance, LED color), reusing the
	// exact logic of the legacy obsolete-part flow.
	if (!silentItems.isEmpty()) {
		MainWindow* mainWindow = findMainWindow();
		if (mainWindow != nullptr) {
			mainWindow->swapObsolete(false, silentItems);
		} else {
			DebugDialog::debug("MigrationHandler: Could not find MainWindow for silent migration");
		}
	}

	m_pendingMigrations = nonSilentMigrations;

	if (!m_pendingMigrations.isEmpty()) {
		m_currentIndex = 0;
		QTimer::singleShot(100, this, &MigrationHandler::handlePendingMigrationDialog);
	}
}

bool MigrationHandler::hasPendingMigrations() const
{
	return !m_pendingMigrations.isEmpty();
}

void MigrationHandler::clearPendingMigrations()
{
	m_pendingMigrations.clear();
}

QList<HistoryEntry> MigrationHandler::getRelevantHistory(ModelPart* instancePart,
                                                         const QList<HistoryEntry>& allHistory)
{
	// Get silencedDate from instance if available
	QString silencedDateStr = instancePart->localProp("silencedDate").toString();
	QDate baselineDate;

	if (!silencedDateStr.isEmpty()) {
		baselineDate = QDate::fromString(silencedDateStr, Qt::ISODate);
	} else {
		// Use old part's date as baseline
		baselineDate = instancePart->date();
	}

	QList<HistoryEntry> relevant;
	for (const HistoryEntry& entry : allHistory) {
		QDate entryDate = entry.parsedDate();
		if (!baselineDate.isValid() || entryDate > baselineDate) {
			relevant.append(entry);
		}
	}

	DebugDialog::debug(QString("[migration]   getRelevantHistory: baseline=%1 (silencedDate='%2' partDate=%3) -> %4 of %5 entries relevant")
	                   .arg(baselineDate.toString(Qt::ISODate), silencedDateStr, instancePart->date().toString(Qt::ISODate))
	                   .arg(relevant.count()).arg(allHistory.count()));

	return relevant;
}

QString MigrationHandler::computeEffectiveMode(const QList<HistoryEntry>& history)
{
	// Most restrictive mode wins: forced > ask > silent
	bool hasForced = false;
	bool hasAsk = false;

	for (const HistoryEntry& entry : history) {
		if (entry.isForced()) hasForced = true;
		if (entry.isAsk()) hasAsk = true;
	}

	if (hasForced) return "forced";
	if (hasAsk) return "ask";
	return "silent";
}

void MigrationHandler::handlePendingMigrationDialog()
{
	if (m_pendingMigrations.isEmpty()) return;

	createMigrationDialog();
	updateDialogForCurrentMigration();

	m_dialog->show();
	m_dialog->raise();
	m_dialog->activateWindow();
}

void MigrationHandler::createMigrationDialog()
{
	if (m_dialog) {
		m_dialog->close();
		m_dialog->deleteLater();
	}

	m_dialog = new QDialog(qobject_cast<QWidget*>(m_sketchWidget));
	m_dialog->setWindowTitle(tr("Part Migration"));
	m_dialog->setModal(false);
	m_dialog->resize(500, 400);
	m_dialog->setAttribute(Qt::WA_DeleteOnClose);

	QVBoxLayout* layout = new QVBoxLayout(m_dialog);

	// Counter label
	m_counterLabel = new QLabel(m_dialog);
	m_counterLabel->setAlignment(Qt::AlignCenter);
	QFont titleFont = m_counterLabel->font();
	titleFont.setBold(true);
	titleFont.setPointSize(titleFont.pointSize() + 2);
	m_counterLabel->setFont(titleFont);
	layout->addWidget(m_counterLabel);

	// Title/part name label
	m_titleLabel = new QLabel(m_dialog);
	m_titleLabel->setWordWrap(true);
	m_titleLabel->setAlignment(Qt::AlignTop);
	layout->addWidget(m_titleLabel);

	// History entries (scrollable)
	QScrollArea* scrollArea = new QScrollArea(m_dialog);
	scrollArea->setWidgetResizable(true);
	scrollArea->setMinimumHeight(150);
	m_historyLabel = new QLabel();
	m_historyLabel->setWordWrap(true);
	m_historyLabel->setAlignment(Qt::AlignTop);
	m_historyLabel->setTextFormat(Qt::RichText);
	scrollArea->setWidget(m_historyLabel);
	layout->addWidget(scrollArea);

	layout->addStretch();

	// Action buttons
	QHBoxLayout* actionLayout = new QHBoxLayout();

	m_swapButton = new QPushButton(tr("Swap to New"), m_dialog);
	m_confirmButton = new QPushButton(tr("Confirm"), m_dialog);
	m_skipButton = new QPushButton(tr("Skip"), m_dialog);
	m_silenceButton = new QPushButton(tr("Silence"), m_dialog);

	actionLayout->addStretch();
	actionLayout->addWidget(m_swapButton);
	actionLayout->addWidget(m_confirmButton);
	actionLayout->addWidget(m_skipButton);
	actionLayout->addWidget(m_silenceButton);
	actionLayout->addStretch();

	layout->addLayout(actionLayout);

	// Connect buttons
	connect(m_swapButton, &QPushButton::clicked, this, &MigrationHandler::swapCurrentPart);
	connect(m_confirmButton, &QPushButton::clicked, this, &MigrationHandler::confirmCurrentMigration);
	connect(m_skipButton, &QPushButton::clicked, this, &MigrationHandler::skipCurrentMigration);
	connect(m_silenceButton, &QPushButton::clicked, this, &MigrationHandler::silenceCurrentMigration);
}

void MigrationHandler::updateDialogForCurrentMigration()
{
	if (m_currentIndex < 0 || m_currentIndex >= m_pendingMigrations.size()) {
		closeDialog();
		return;
	}

	const MigrationInfo& info = m_pendingMigrations[m_currentIndex];
	ItemBase* item = currentItem();

	if (!item) {
		DebugDialog::debug("MigrationHandler: Could not find item for migration");
		processNextMigration();
		return;
	}

	// Update counter
	if (m_pendingMigrations.size() == 1) {
		m_counterLabel->setText(tr("Part Migration"));
	} else {
		m_counterLabel->setText(tr("Part %1 of %2")
		                            .arg(m_currentIndex + 1)
		                            .arg(m_pendingMigrations.size()));
	}

	// Update title
	QString title = QString("<b>%1</b>").arg(info.instanceTitle);
	if (info.isSwapped) {
		title += QString(" <i>(%1)</i>").arg(tr("showing new version"));
	} else {
		title += QString(" <i>(%1)</i>").arg(tr("showing old version"));
	}
	m_titleLabel->setText(title);

	// Update history entries
	QString historyHtml = QString("<p><b>%1</b></p>").arg(tr("Changes since your version:"));
	for (const HistoryEntry& entry : info.relevantHistory) {
		QString modeTag;
		if (entry.isForced()) {
			modeTag = QString(" <span style='color: red;'>[%1]</span>").arg(tr("REQUIRED"));
		} else if (entry.isSilent()) {
			modeTag = QString(" <span style='color: gray;'>[%1]</span>").arg(tr("AUTO"));
		}

		historyHtml += QString("<p><b>%1</b> (%2):%3<br/>%4</p>")
		                   .arg(entry.date)
		                   .arg(entry.author)
		                   .arg(modeTag)
		                   .arg(entry.description);
	}
	m_historyLabel->setText(historyHtml);

	// Update button states
	m_swapButton->setText(info.isSwapped ? tr("Swap to Old") : tr("Swap to New"));
	m_silenceButton->setVisible(info.effectiveMode == "ask");

	// Focus on the part
	centerAndZoomOnItem(item);
}

void MigrationHandler::centerAndZoomOnItem(ItemBase* item)
{
	if (!item) return;

	MainWindow* mainWindow = findMainWindow();
	if (mainWindow == nullptr) return;

	// Bring the view that actually contains this part to the front, then zoom it
	// so the user sees the part being migrated regardless of which view they were on.
	mainWindow->setCurrentView(item->viewID());
	SketchWidget* view = mainWindow->sketchWidgetForView(item->viewID());
	if (view == nullptr) return;

	QPointF itemPos = item->pos();
	QRectF focusRect(itemPos.x() - 200, itemPos.y() - 200, 400, 400);

	view->fitInView(focusRect, Qt::KeepAspectRatio);
	view->updateZoomFromCurrentTransform();

	view->scene()->clearSelection();
	item->setSelected(true);
}

void MigrationHandler::swapCurrentPart()
{
	if (m_currentIndex < 0 || m_currentIndex >= m_pendingMigrations.size()) return;

	MigrationInfo& info = m_pendingMigrations[m_currentIndex];

	MainWindow* mainWindow = findMainWindow();
	if (!mainWindow) {
		DebugDialog::debug("MigrationHandler: Could not find MainWindow");
		return;
	}

	if (info.isSwapped) {
		// Currently showing new part, undo to get back to old.
		// The undo stack is shared across all views, so undoing here is view-independent.
		if (m_sketchWidget->undoStack()) {
			m_sketchWidget->undoStack()->undo();
		}
		info.itemId = info.originalItemId;  // Restore original item ID
		info.isSwapped = false;
	} else {
		// Currently showing old part, swap to new
		ItemBase* item = currentItem();
		if (!item) {
			DebugDialog::debug("MigrationHandler: Could not find item for swap");
			return;
		}

		// Work in the view that actually contains the part (it may not be PCB).
		SketchWidget* view = mainWindow->sketchWidgetForView(item->viewID());
		if (view == nullptr) return;

		// Select the item before swap (swapSelectedAux works on selection)
		view->scene()->clearSelection();
		item->setSelected(true);

		QMap<QString, QString> propsMap;
		mainWindow->swapSelectedAux(item, info.newModuleID, false,
		                            item->viewLayerPlacement(), propsMap);

		// After swap, the new item should be selected - get its ID (same view)
		QList<QGraphicsItem*> selected = view->scene()->selectedItems();
		for (QGraphicsItem* gi : selected) {
			ItemBase* newItem = dynamic_cast<ItemBase*>(gi);
			if (newItem && newItem->instanceTitle() == info.instanceTitle) {
				info.itemId = newItem->id();
				break;
			}
		}

		info.isSwapped = true;
	}

	updateDialogForCurrentMigration();
}

void MigrationHandler::confirmCurrentMigration()
{
	if (m_currentIndex < 0 || m_currentIndex >= m_pendingMigrations.size()) return;

	MigrationInfo& info = m_pendingMigrations[m_currentIndex];

	// If not already swapped, perform the swap first
	if (!info.isSwapped) {
		swapCurrentPart();
	}

	// Move to next migration
	processNextMigration();
}

void MigrationHandler::skipCurrentMigration()
{
	if (m_currentIndex < 0 || m_currentIndex >= m_pendingMigrations.size()) return;

	MigrationInfo& info = m_pendingMigrations[m_currentIndex];

	// If currently showing new part, swap back to old
	if (info.isSwapped) {
		swapCurrentPart();
	}

	// Move to next migration
	processNextMigration();
}

void MigrationHandler::silenceCurrentMigration()
{
	if (m_currentIndex < 0 || m_currentIndex >= m_pendingMigrations.size()) return;

	MigrationInfo& info = m_pendingMigrations[m_currentIndex];

	// Can only silence "ask" mode
	if (info.effectiveMode != "ask") return;

	// If currently showing new part, swap back to old (silence means keep old)
	if (info.isSwapped) {
		swapCurrentPart();
	}

	// Set silencedDate on the instance to the latest history entry date
	QString latestDate;
	for (const HistoryEntry& entry : info.relevantHistory) {
		if (latestDate.isEmpty() || entry.date > latestDate) {
			latestDate = entry.date;
		}
	}

	if (!latestDate.isEmpty()) {
		ItemBase* item = currentItem();
		if (item && item->modelPart()) {
			item->modelPart()->setLocalProp("silencedDate", latestDate);
			DebugDialog::debug(QString("Set silencedDate to %1 for %2")
			                       .arg(latestDate)
			                       .arg(info.instanceTitle));
		}
	}

	// Move to next migration
	processNextMigration();
}

void MigrationHandler::processNextMigration()
{
	m_currentIndex++;
	if (m_currentIndex >= m_pendingMigrations.size()) {
		closeDialog();
	} else {
		updateDialogForCurrentMigration();
	}
}

void MigrationHandler::closeDialog()
{
	if (m_dialog) {
		m_dialog->close();
		m_dialog = nullptr;
	}
	m_pendingMigrations.clear();
	m_currentIndex = 0;
}

MainWindow* MigrationHandler::findMainWindow() const
{
	QWidget* widget = qobject_cast<QWidget*>(m_sketchWidget);
	while (widget) {
		MainWindow* mainWindow = qobject_cast<MainWindow*>(widget);
		if (mainWindow) return mainWindow;
		widget = widget->parentWidget();
	}
	return nullptr;
}
