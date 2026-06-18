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
#include "../commands.h"

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
	, m_reasonLabel(nullptr)
	, m_historyLabel(nullptr)
	, m_swapButton(nullptr)
	, m_confirmButton(nullptr)
	, m_skipButton(nullptr)
	, m_silenceButton(nullptr)
	, m_prevButton(nullptr)
	, m_nextButton(nullptr)
{
}

void MigrationHandler::queueMigration(ItemBase* itemBase, ModelPart* oldPart,
                                      ModelPart* newPart, const QList<HistoryEntry>& history,
                                      const QString& reason)
{
	MigrationInfo info;
	info.itemId = itemBase->id();
	info.originalItemId = itemBase->id();
	info.oldModuleID = oldPart->moduleID();
	info.newModuleID = newPart->moduleID();
	info.instanceTitle = itemBase->instanceTitle();
	info.relevantHistory = history;
	// Classify from the full history (the part's nature), not just the unseen subset:
	// an all-silent part with no unseen entries must still resolve to "silent", and a
	// history-less part to "forced".
	info.effectiveMode = computeEffectiveMode(newPart != nullptr ? newPart->history() : history);
	info.isSwapped = false;
	info.reason = reason;

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

	// Auto-apply all silent migrations as a single undoable command, with no dialog and no
	// feedback popup. Use swapObsoleteDirect() (not swapObsolete()) so we don't re-enter
	// routeHistoryMigrations and re-queue these same items. It ports the special-cased
	// properties (resistance, LED colour), reusing the legacy obsolete-part flow.
	if (!silentItems.isEmpty()) {
		MainWindow* mainWindow = findMainWindow();
		if (mainWindow != nullptr) {
			mainWindow->swapObsoleteDirect(silentItems, false);
			// Silent swaps change the sketch without a dialog; tell the user non-modally.
			// (The new part's history remains visible in the Inspector "Revisions" section.)
			mainWindow->statusMessage(
			    tr("%n part(s) were automatically updated to a newer version", "", silentItems.count()),
			    5000);
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
	// Baseline = the part's stored silence date if present, else the part's own date. Guard the
	// parse: an unparseable silencedDate must fall back to the part date, not leave the baseline
	// invalid (which would make every entry look unseen and re-prompt forever).
	QString silencedDateStr = instancePart->localProp("silencedDate").toString();
	QDate baselineDate;
	if (!silencedDateStr.isEmpty()) {
		baselineDate = QDate::fromString(silencedDateStr, Qt::ISODate);
	}
	if (!baselineDate.isValid()) {
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
	// No migration notes at all → classic hard obsoletion: prompt on every load.
	// (Only an explicit mode="silent" history entry opts a part into auto-swap.)
	if (history.isEmpty()) return "forced";

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

	if (m_dialog) {
		m_dialog->show();
		m_dialog->raise();
		m_dialog->activateWindow();
	}
}

void MigrationHandler::createMigrationDialog()
{
	// Defensive: if a previous dialog somehow lingers, tear it down without triggering our
	// finished handler (which would wipe the fresh migration session we're about to show).
	if (m_dialog) {
		disconnect(m_dialog, nullptr, this, nullptr);
		m_dialog->close();
	}

	m_dialog = new QDialog(qobject_cast<QWidget*>(m_sketchWidget));
	m_dialog->setWindowTitle(tr("Part Migration"));
	m_dialog->setObjectName("partMigrationDialog");   // for GUI test probes
	m_dialog->setModal(false);
	m_dialog->resize(500, 400);
	m_dialog->setAttribute(Qt::WA_DeleteOnClose);
	// Closing by any means (ESC, window close, or completion) runs cleanup. m_dialog is a
	// QPointer, so it auto-nulls when the dialog is deleted — no dangling reuse on the next open.
	connect(m_dialog, &QDialog::finished, this, &MigrationHandler::onDialogClosed);

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

	// Reason / explanation label (why this dialog is being shown)
	m_reasonLabel = new QLabel(m_dialog);
	m_reasonLabel->setWordWrap(true);
	m_reasonLabel->setAlignment(Qt::AlignTop);
	layout->addWidget(m_reasonLabel);

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

	// Stable object names so GUI tests can find these buttons regardless of label translation.
	m_swapButton->setObjectName("migrationSwapButton");
	m_confirmButton->setObjectName("migrationConfirmButton");
	m_skipButton->setObjectName("migrationSkipButton");
	m_silenceButton->setObjectName("migrationSilenceButton");

	// Tooltips (the swap button's is set per-state in updateDialogForCurrentMigration).
	m_confirmButton->setToolTip(tr("Update to the new version and continue to the next part"));
	m_skipButton->setToolTip(tr("Keep the old version for now and continue (you may be asked again later)"));
	m_silenceButton->setToolTip(tr("Keep the old version and don't ask about these changes again"));

	actionLayout->addStretch();
	actionLayout->addWidget(m_swapButton);
	actionLayout->addWidget(m_confirmButton);
	actionLayout->addWidget(m_skipButton);
	actionLayout->addWidget(m_silenceButton);
	actionLayout->addStretch();

	layout->addLayout(actionLayout);

	// Navigation between parts (only shown when there are several), with a bulk "Update all".
	QHBoxLayout* navLayout = new QHBoxLayout();
	m_prevButton = new QPushButton(tr("Previous"), m_dialog);
	m_nextButton = new QPushButton(tr("Next"), m_dialog);
	m_updateAllButton = new QPushButton(tr("Update all"), m_dialog);
	m_prevButton->setObjectName("migrationPrevButton");
	m_nextButton->setObjectName("migrationNextButton");
	m_updateAllButton->setObjectName("migrationUpdateAllButton");
	m_prevButton->setToolTip(tr("Go back to the previous part"));
	m_nextButton->setToolTip(tr("Go to the next part"));
	m_updateAllButton->setToolTip(tr("Update every outdated part in this list to its newest version"));
	navLayout->addWidget(m_prevButton);
	navLayout->addStretch();
	navLayout->addWidget(m_updateAllButton);
	navLayout->addStretch();
	navLayout->addWidget(m_nextButton);
	layout->addLayout(navLayout);

	// Connect buttons
	connect(m_swapButton, &QPushButton::clicked, this, &MigrationHandler::swapCurrentPart);
	connect(m_confirmButton, &QPushButton::clicked, this, &MigrationHandler::confirmCurrentMigration);
	connect(m_skipButton, &QPushButton::clicked, this, &MigrationHandler::skipCurrentMigration);
	connect(m_silenceButton, &QPushButton::clicked, this, &MigrationHandler::silenceCurrentMigration);
	connect(m_updateAllButton, &QPushButton::clicked, this, &MigrationHandler::updateAllMigrations);
	connect(m_prevButton, &QPushButton::clicked, this, &MigrationHandler::goToPreviousMigration);
	connect(m_nextButton, &QPushButton::clicked, this, &MigrationHandler::goToNextMigration);
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
		// Mark it decided so the wrap-around advance doesn't loop back to it.
		m_pendingMigrations[m_currentIndex].decided = true;
		processNextMigration();
		return;
	}

	// Update counter. Don't repeat the window title ("Part Migration") in the content;
	// only show a counter when there are several parts to step through.
	if (m_pendingMigrations.size() == 1) {
		m_counterLabel->setVisible(false);
	} else {
		m_counterLabel->setVisible(true);
		m_counterLabel->setText(tr("Part %1 of %2")
		                            .arg(m_currentIndex + 1)
		                            .arg(m_pendingMigrations.size()));
	}

	// Update title
	QString title = QString("<b>%1</b>").arg(info.instanceTitle);
	if (info.decided) {
		QString outcome = info.silenced ? tr("silenced — keeping old version")
		                  : info.isSwapped ? tr("updated to new version")
		                  : tr("keeping old version");
		title += QString(" — <i>%1</i>").arg(outcome);
	} else if (info.isSwapped) {
		title += QString(" <i>(%1)</i>").arg(tr("showing new version"));
	} else {
		title += QString(" <i>(%1)</i>").arg(tr("showing old version"));
	}
	m_titleLabel->setText(title);

	// Explanation of why this dialog is being shown (e.g. mixed versions)
	m_reasonLabel->setText(info.reason);
	m_reasonLabel->setVisible(!info.reason.isEmpty());

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
	m_swapButton->setToolTip(info.isSwapped
	                         ? tr("Revert the preview and show the old version again")
	                         : tr("Preview the new version in the sketch (you can switch back)"));
	// Silence (persistent "don't ask again") is offered only for soft "ask" migrations;
	// classic "forced" parts must keep prompting, so they get Update/Skip only.
	m_silenceButton->setVisible(info.effectiveMode == "ask");

	// Every part stays actionable, including ones already decided: revisiting a part and
	// changing the choice is handled by the swap/revert logic (clean undo when possible,
	// otherwise a fresh swap), so there is no read-only state.
	m_swapButton->setEnabled(true);
	m_confirmButton->setEnabled(true);
	m_skipButton->setEnabled(true);
	m_silenceButton->setEnabled(true);

	// Navigation between parts: only when there are several to step through.
	int count = m_pendingMigrations.size();
	m_prevButton->setVisible(count > 1);
	m_nextButton->setVisible(count > 1);
	m_updateAllButton->setVisible(count > 1);
	m_prevButton->setEnabled(m_currentIndex > 0);
	m_nextButton->setEnabled(m_currentIndex < count - 1);

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
	if (info.isSwapped) revertToOld(info);
	else swapToNew(info);

	updateDialogForCurrentMigration();
}

qint64 MigrationHandler::findSwappedItemId(SketchWidget* view, const QString& instanceTitle, qint64 fallback) const
{
	// After a swap the new item is selected in its view; identify it by instance title.
	Q_FOREACH (QGraphicsItem* gi, view->scene()->selectedItems()) {
		ItemBase* it = dynamic_cast<ItemBase*>(gi);
		if (it != nullptr && it->instanceTitle() == instanceTitle) return it->id();
	}
	return fallback;
}

void MigrationHandler::swapToNew(MigrationInfo& info)
{
	MainWindow* mainWindow = findMainWindow();
	if (!mainWindow) {
		DebugDialog::debug("MigrationHandler: Could not find MainWindow");
		return;
	}
	ItemBase* item = currentItem();
	if (!item) {
		DebugDialog::debug("MigrationHandler: Could not find item for swap");
		return;
	}
	// Work in the view that actually contains the part (it may not be PCB).
	SketchWidget* view = mainWindow->sketchWidgetForView(item->viewID());
	if (view == nullptr) return;

	// Remember the old item so a clean undo can restore exactly it.
	info.originalItemId = item->id();

	// swapSelectedAux works on the current selection.
	view->scene()->clearSelection();
	item->setSelected(true);

	QMap<QString, QString> propsMap;
	mainWindow->swapSelectedAux(item, info.newModuleID, false, item->viewLayerPlacement(), propsMap);

	// One top-level command was pushed; remember where, so we can tell later whether our swap
	// is still safely undoable or whether the user has pushed real commands on top of it.
	info.swapStackIndex = m_sketchWidget->undoStack() ? m_sketchWidget->undoStack()->index() : -1;
	info.itemId = findSwappedItemId(view, info.instanceTitle, info.itemId);
	info.isSwapped = true;
}

bool MigrationHandler::canUndoOwnSwap(int swapStackIndex) const
{
	// Our swap is safe to undo only if it's still effectively on top: nothing was pushed since,
	// or the only commands pushed since are selection changes (no real edit). Anything else means
	// the user made edits we must not rewind, so the caller should swap forward instead.
	if (swapStackIndex < 0) return false;
	WaitPushUndoStack* stack = m_sketchWidget->undoStack();
	if (stack == nullptr) return false;
	int idx = stack->index();
	if (idx < swapStackIndex) return false;   // user already undid past our swap
	for (int i = swapStackIndex; i < idx; ++i) {
		if (dynamic_cast<const SelectItemCommand*>(stack->command(i)) == nullptr) return false;
	}
	return true;
}

void MigrationHandler::revertToOld(MigrationInfo& info)
{
	WaitPushUndoStack* stack = m_sketchWidget->undoStack();

	if (canUndoOwnSwap(info.swapStackIndex) && stack != nullptr) {
		// Undo any selection-only commands stacked on top of our swap, then the swap itself.
		int steps = stack->index() - info.swapStackIndex + 1;
		for (int i = 0; i < steps; ++i) stack->undo();
		info.itemId = info.originalItemId;   // undo restores the original old item (same id)
	}
	else {
		// Real commands intervened (or the swap is gone): don't rewind history. Swap the part
		// forward from new back to old as a fresh, independent command.
		MainWindow* mainWindow = findMainWindow();
		ItemBase* item = currentItem();
		if (mainWindow != nullptr && item != nullptr) {
			SketchWidget* view = mainWindow->sketchWidgetForView(item->viewID());
			if (view != nullptr) {
				view->scene()->clearSelection();
				item->setSelected(true);
				QMap<QString, QString> propsMap;
				mainWindow->swapSelectedAux(item, info.oldModuleID, false, item->viewLayerPlacement(), propsMap);
				info.itemId = findSwappedItemId(view, info.instanceTitle, info.itemId);
			}
		}
	}

	info.swapStackIndex = -1;
	info.isSwapped = false;
}

void MigrationHandler::confirmCurrentMigration()
{
	if (m_currentIndex < 0 || m_currentIndex >= m_pendingMigrations.size()) return;

	MigrationInfo& info = m_pendingMigrations[m_currentIndex];

	// Commit the new version (swap now if the user didn't preview it first).
	if (!info.isSwapped) {
		swapToNew(info);
	}

	info.decided = true;
	info.silenced = false;
	// Move to the next undecided part (closes when all are decided)
	processNextMigration();
}

void MigrationHandler::skipCurrentMigration()
{
	if (m_currentIndex < 0 || m_currentIndex >= m_pendingMigrations.size()) return;

	MigrationInfo& info = m_pendingMigrations[m_currentIndex];

	// If currently previewing the new part, revert to the old version.
	if (info.isSwapped) {
		revertToOld(info);
	}

	info.decided = true;
	info.silenced = false;
	// Move to the next undecided part (closes when all are decided)
	processNextMigration();
}

void MigrationHandler::silenceCurrentMigration()
{
	if (m_currentIndex < 0 || m_currentIndex >= m_pendingMigrations.size()) return;

	MigrationInfo& info = m_pendingMigrations[m_currentIndex];

	// Can only silence "ask" mode
	if (info.effectiveMode != "ask") return;

	// If currently previewing the new part, revert to the old version (silence means keep old).
	if (info.isSwapped) {
		revertToOld(info);
	}

	// Baseline the silence at the newest relevant entry, stored ISO-normalised so it round-trips
	// and parses back reliably in getRelevantHistory.
	QDate latest;
	for (const HistoryEntry& entry : info.relevantHistory) {
		QDate d = entry.parsedDate();
		if (d.isValid() && (!latest.isValid() || d > latest)) latest = d;
	}

	if (latest.isValid()) {
		ItemBase* item = currentItem();
		if (item && item->modelPart()) {
			QString isoDate = latest.toString(Qt::ISODate);
			QString oldDate = item->modelPart()->localProp("silencedDate").toString();
			MainWindow* mainWindow = findMainWindow();
			SketchWidget* view = (mainWindow != nullptr) ? mainWindow->sketchWidgetForView(item->viewID()) : nullptr;
			if (view != nullptr && view->undoStack() != nullptr) {
				// Push as a command so the sketch is marked modified (and prompts to save) and
				// the choice is undoable. SetPropCommand routes to the same modelPart localProp.
				auto* cmd = new SetPropCommand(view, item->id(), "silencedDate", oldDate, isoDate, false, nullptr);
				cmd->setText(tr("Silence update reminder for %1").arg(info.instanceTitle));
				view->undoStack()->push(cmd);
			} else {
				item->modelPart()->setLocalProp("silencedDate", isoDate);
			}
			DebugDialog::debug(QString("Set silencedDate to %1 for %2").arg(isoDate, info.instanceTitle));
		}
	}

	info.decided = true;
	info.silenced = true;
	// Move to the next undecided part (closes when all are decided)
	processNextMigration();
}

void MigrationHandler::processNextMigration()
{
	// Advance to the next part the user hasn't decided yet (wrapping around); close
	// the dialog once every part has a decision.
	int count = m_pendingMigrations.size();
	for (int step = 1; step <= count; ++step) {
		int idx = (m_currentIndex + step) % count;
		if (!m_pendingMigrations[idx].decided) {
			m_currentIndex = idx;
			updateDialogForCurrentMigration();
			return;
		}
	}
	closeDialog();
}

void MigrationHandler::revertCurrentPreviewIfTransient()
{
	if (m_currentIndex < 0 || m_currentIndex >= m_pendingMigrations.size()) return;

	MigrationInfo& info = m_pendingMigrations[m_currentIndex];
	// Only revert an undecided live preview. A part that has been decided keeps its committed state.
	if (info.isSwapped && !info.decided) {
		revertToOld(info);
	}
}

void MigrationHandler::goToPreviousMigration()
{
	if (m_currentIndex <= 0) return;
	revertCurrentPreviewIfTransient();
	m_currentIndex--;
	updateDialogForCurrentMigration();
}

void MigrationHandler::goToNextMigration()
{
	if (m_currentIndex >= m_pendingMigrations.size() - 1) return;
	revertCurrentPreviewIfTransient();
	m_currentIndex++;
	updateDialogForCurrentMigration();
}

void MigrationHandler::updateAllMigrations()
{
	MainWindow* mainWindow = findMainWindow();
	if (mainWindow == nullptr) { closeDialog(); return; }

	// Normalise every part back to its old version (reverting any live preview), then swap all
	// that are still obsolete to their new version in a single undoable command.
	QList<ItemBase*> toSwap;
	for (int i = 0; i < m_pendingMigrations.size(); ++i) {
		m_currentIndex = i;
		MigrationInfo& info = m_pendingMigrations[i];
		if (info.isSwapped && !info.decided) revertToOld(info);
		ItemBase* item = currentItem(i);
		if (item != nullptr && item->isObsolete()) toSwap << item;
		info.decided = true;
		info.silenced = false;
	}

	if (!toSwap.isEmpty()) {
		mainWindow->swapObsoleteDirect(toSwap, false);
	}
	closeDialog();
}

void MigrationHandler::closeDialog()
{
	// Programmatic close (completion / Update all). A non-modal QDialog::close() doesn't reliably
	// emit finished(), so disconnect our handler and run the cleanup ourselves. WA_DeleteOnClose
	// still deletes the dialog; m_dialog (QPointer) auto-nulls afterwards.
	if (m_dialog) {
		disconnect(m_dialog, nullptr, this, nullptr);
		m_dialog->close();
	}
	onDialogClosed();
}

void MigrationHandler::onDialogClosed()
{
	// The dialog closed (ESC, window close, "Update all", or normal completion). Revert any
	// still-undecided live preview, then drop the session state. The dialog deletes itself
	// (WA_DeleteOnClose) and m_dialog (a QPointer) auto-nulls, so a later open starts clean.
	revertCurrentPreviewIfTransient();
	m_pendingMigrations.clear();
	m_currentIndex = 0;

	// These widgets are children of the dialog and are being destroyed with it; drop the
	// dangling raw pointers so nothing touches them before the next createMigrationDialog().
	m_counterLabel = nullptr;
	m_titleLabel = nullptr;
	m_reasonLabel = nullptr;
	m_historyLabel = nullptr;
	m_swapButton = nullptr;
	m_confirmButton = nullptr;
	m_skipButton = nullptr;
	m_silenceButton = nullptr;
	m_updateAllButton = nullptr;
	m_prevButton = nullptr;
	m_nextButton = nullptr;
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
