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
#include "../viewlayer.h"

#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QButtonGroup>
#include <QGroupBox>
#include <QSignalBlocker>
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
	, m_versionGroup(nullptr)
	, m_oldRadio(nullptr)
	, m_newRadio(nullptr)
	, m_silenceButton(nullptr)
	, m_updateAllButton(nullptr)
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
	info.oldTitle = oldPart->title();
	info.oldVersion = oldPart->version();
	if (newPart != nullptr) {
		info.newTitle = newPart->title();
		info.newVersion = newPart->version();
	}
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

	// Parent to the MainWindow (not the sketch view): Fritzing sets its stylesheet per-MainWindow
	// rather than app-wide, so this makes the dialog inherit the app styling, and it centres on
	// the main window. Fall back to the sketch widget if the window can't be resolved.
	MainWindow* parentWindow = findMainWindow();
	m_dialog = new QDialog(parentWindow != nullptr ? static_cast<QWidget*>(parentWindow)
	                                               : qobject_cast<QWidget*>(m_sketchWidget));
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
	m_counterLabel->setObjectName("migrationCounterLabel");
	m_counterLabel->setAlignment(Qt::AlignCenter);
	QFont titleFont = m_counterLabel->font();
	titleFont.setBold(true);
	titleFont.setPointSize(titleFont.pointSize() + 2);
	m_counterLabel->setFont(titleFont);
	layout->addWidget(m_counterLabel);

	// Title/part name label
	m_titleLabel = new QLabel(m_dialog);
	m_titleLabel->setObjectName("migrationTitleLabel");
	m_titleLabel->setWordWrap(true);
	m_titleLabel->setAlignment(Qt::AlignTop);
	layout->addWidget(m_titleLabel);

	// Reason / explanation label (why this dialog is being shown)
	m_reasonLabel = new QLabel(m_dialog);
	m_reasonLabel->setObjectName("migrationReasonLabel");
	m_reasonLabel->setWordWrap(true);
	m_reasonLabel->setAlignment(Qt::AlignTop);
	layout->addWidget(m_reasonLabel);

	// History entries (scrollable)
	QScrollArea* scrollArea = new QScrollArea(m_dialog);
	scrollArea->setWidgetResizable(true);
	scrollArea->setMinimumHeight(150);
	m_historyLabel = new QLabel();
	m_historyLabel->setObjectName("migrationHistoryLabel");
	m_historyLabel->setWordWrap(true);
	m_historyLabel->setAlignment(Qt::AlignTop);
	m_historyLabel->setTextFormat(Qt::RichText);
	scrollArea->setWidget(m_historyLabel);
	layout->addWidget(scrollArea);

	layout->addStretch();

	// Version chooser: two radios (old / new), each labelled with the part name + version.
	// Toggling swaps the part live; it is preset to the currently active version per part.
	auto* versionBox = new QGroupBox(tr("Version"), m_dialog);
	auto* versionLayout = new QVBoxLayout(versionBox);
	m_oldRadio = new QRadioButton(versionBox);
	m_newRadio = new QRadioButton(versionBox);
	m_oldRadio->setObjectName("migrationOldRadio");
	m_newRadio->setObjectName("migrationNewRadio");
	m_versionGroup = new QButtonGroup(versionBox);   // owned by the dialog, dies with it
	m_versionGroup->setExclusive(true);
	m_versionGroup->addButton(m_oldRadio);
	m_versionGroup->addButton(m_newRadio);
	versionLayout->addWidget(m_oldRadio);
	versionLayout->addWidget(m_newRadio);
	layout->addWidget(versionBox);

	// Action buttons
	QHBoxLayout* actionLayout = new QHBoxLayout();

	m_silenceButton = new QPushButton(tr("Silence"), m_dialog);
	m_silenceButton->setObjectName("migrationSilenceButton");
	m_silenceButton->setToolTip(tr("Keep the old version and don't ask about these changes again"));

	actionLayout->addStretch();
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
	connect(m_newRadio, &QRadioButton::toggled, this, &MigrationHandler::onVersionToggled);
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

	MigrationInfo& info = m_pendingMigrations[m_currentIndex];

	// First time we land on a part, default to the new version (swap live). After that the part
	// is "visited", so navigating away and back keeps whatever the user last chose.
	if (!info.visited) {
		info.visited = true;
		if (currentItem() != nullptr) swapToNew(info);
	}

	ItemBase* item = currentItem();
	if (!item) {
		DebugDialog::debug("MigrationHandler: Could not find item for migration");
		// Mark it decided so the wrap-around advance doesn't loop back to it.
		info.decided = true;
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

	// Content area: the change notes, or — when there are none — the names (if they differ)
	// plus a hint to compare the two versions visually across the views.
	QString contentHtml;
	if (!info.relevantHistory.isEmpty()) {
		contentHtml = QString("<p><b>%1</b></p>").arg(tr("Changes since your version:"));
		for (const HistoryEntry& entry : info.relevantHistory) {
			QString modeTag;
			if (entry.isForced()) {
				modeTag = QString(" <span style='color: red;'>[%1]</span>").arg(tr("REQUIRED"));
			} else if (entry.isSilent()) {
				modeTag = QString(" <span style='color: gray;'>[%1]</span>").arg(tr("AUTO"));
			}

			contentHtml += QString("<p><b>%1</b> (%2):%3<br/>%4</p>")
			                   .arg(entry.date.toHtmlEscaped())
			                   .arg(entry.author.toHtmlEscaped())
			                   .arg(modeTag)
			                   .arg(entry.description.toHtmlEscaped());
		}
	} else {
		if (!info.newTitle.isEmpty() && info.newTitle != info.oldTitle) {
			contentHtml += QString("<p>%1 &rarr; %2</p>")
			                   .arg(info.oldTitle.toHtmlEscaped(), info.newTitle.toHtmlEscaped());
		}
		contentHtml += QString("<p><i>%1</i></p>").arg(
		    tr("No change notes are available. Compare the old and new version visually in the "
		       "Breadboard, Schematic and PCB views before deciding."));
	}
	m_historyLabel->setText(contentHtml);

	// Version chooser: label each radio with the part name + version, and preset (without
	// triggering a swap) to whichever version is currently active for this part.
	{
		QSignalBlocker blockOld(m_oldRadio);
		QSignalBlocker blockNew(m_newRadio);
		m_oldRadio->setText(info.oldVersion.isEmpty()
		                    ? tr("Keep old: %1").arg(info.oldTitle)
		                    : tr("Keep old: %1 (v. %2)").arg(info.oldTitle, info.oldVersion));
		m_newRadio->setText(info.newVersion.isEmpty()
		                    ? tr("Use new: %1").arg(info.newTitle)
		                    : tr("Use new: %1 (v. %2)").arg(info.newTitle, info.newVersion));
		m_oldRadio->setChecked(!info.isSwapped);
		m_newRadio->setChecked(info.isSwapped);
	}

	// Silence (persistent "don't ask again") is offered only for soft "ask" migrations;
	// classic "forced" parts must keep prompting, so they get the chooser + Skip only.
	m_silenceButton->setVisible(info.effectiveMode == "ask");
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

void MigrationHandler::onVersionToggled(bool useNew)
{
	// Driven by the "new" radio. This is the only place a part is swapped/reverted, so navigating
	// between parts never changes anything until the user actually flips the toggle here.
	if (m_currentIndex < 0 || m_currentIndex >= m_pendingMigrations.size()) return;

	MigrationInfo& info = m_pendingMigrations[m_currentIndex];
	DebugDialog::debug(QString("[migration-dbg] onVersionToggled idx=%1 '%2' useNew=%3 isSwapped=%4 visited=%5 itemId=%6")
	                   .arg(m_currentIndex).arg(info.instanceTitle, useNew ? "yes" : "no",
	                        info.isSwapped ? "yes" : "no", info.visited ? "yes" : "no")
	                   .arg(info.itemId));
	if (useNew && !info.isSwapped) swapToNew(info);
	else if (!useNew && info.isSwapped) revertToOld(info);
	info.decided = true;

	updateDialogForCurrentMigration();
}

qint64 MigrationHandler::findSwappedItemId(SketchWidget* view, const QString& instanceTitle, qint64 fallback) const
{
	// After a swap the new item is selected in its view; identify it by instance title.
	int selCount = view->scene()->selectedItems().count();
	qint64 found = -1;
	Q_FOREACH (QGraphicsItem* gi, view->scene()->selectedItems()) {
		ItemBase* it = dynamic_cast<ItemBase*>(gi);
		if (it == nullptr) continue;
		DebugDialog::debug(QString("[migration-dbg]   selected id=%1 title='%2' module=%3 chiefId=%4")
		                   .arg(it->id()).arg(it->instanceTitle(), it->moduleID())
		                   .arg(it->layerKinChief() ? it->layerKinChief()->id() : -1));
		if (found < 0 && it->instanceTitle() == instanceTitle) found = it->id();
	}
	if (found < 0) {
		DebugDialog::debug(QString("[migration-dbg]   findSwappedItemId: NO match for '%1' among %2 selected -> FALLBACK to id=%3")
		                   .arg(instanceTitle).arg(selCount).arg(fallback));
		return fallback;
	}
	DebugDialog::debug(QString("[migration-dbg]   findSwappedItemId: matched '%1' -> id=%2 (of %3 selected)")
	                   .arg(instanceTitle).arg(found).arg(selCount));
	return found;
}

void MigrationHandler::debugDumpInstances(const QString& tag, const QString& instanceTitle) const
{
	MainWindow* mainWindow = findMainWindow();
	if (mainWindow == nullptr) return;

	const ViewLayer::ViewID viewIds[] = { ViewLayer::BreadboardView, ViewLayer::SchematicView, ViewLayer::PCBView };
	const char* viewNames[] = { "bb", "schem", "pcb" };
	for (int v = 0; v < 3; ++v) {
		SketchWidget* view = mainWindow->sketchWidgetForView(viewIds[v]);
		if (view == nullptr) continue;
		int matchCount = 0;
		Q_FOREACH (QGraphicsItem* gi, view->scene()->items()) {
			ItemBase* ib = dynamic_cast<ItemBase*>(gi);
			if (ib == nullptr || ib->instanceTitle() != instanceTitle) continue;
			++matchCount;
			ItemBase* chief = ib->layerKinChief();
			DebugDialog::debug(QString("[migration-dbg]   [%1] %2 id=%3 module=%4 chiefId=%5 isChief=%6 selected=%7")
			                   .arg(tag, QString::fromLatin1(viewNames[v]))
			                   .arg(ib->id()).arg(ib->moduleID())
			                   .arg(chief ? chief->id() : -1)
			                   .arg(chief == ib ? "yes" : "no")
			                   .arg(ib->isSelected() ? "yes" : "no"));
		}
		DebugDialog::debug(QString("[migration-dbg]   [%1] %2 matchCount=%3")
		                   .arg(tag, QString::fromLatin1(viewNames[v])).arg(matchCount));
	}
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

	WaitPushUndoStack* stack = m_sketchWidget->undoStack();
	DebugDialog::debug(QString("[migration-dbg] swapToNew '%1' old=%2 -> new=%3 viewItemId=%4 view=%5 undoIdx(before)=%6")
	                   .arg(info.instanceTitle, info.oldModuleID, info.newModuleID)
	                   .arg(item->id()).arg(int(item->viewID())).arg(stack ? stack->index() : -1));
	debugDumpInstances("swapToNew:before", info.instanceTitle);

	// Remember the old item so a clean undo can restore exactly it.
	info.originalItemId = item->id();

	// swapSelectedAux works on the current selection.
	view->scene()->clearSelection();
	item->setSelected(true);

	QMap<QString, QString> propsMap;
	mainWindow->swapSelectedAux(item, info.newModuleID, false, item->viewLayerPlacement(), propsMap);

	// One top-level command was pushed; remember where, so we can tell later whether our swap
	// is still safely undoable or whether the user has pushed real commands on top of it.
	info.swapStackIndex = stack ? stack->index() : -1;
	info.itemId = findSwappedItemId(view, info.instanceTitle, info.itemId);
	info.isSwapped = true;

	DebugDialog::debug(QString("[migration-dbg] swapToNew done '%1' newItemId=%2 swapStackIndex=%3 originalItemId=%4 undoIdx(after)=%5")
	                   .arg(info.instanceTitle).arg(info.itemId).arg(info.swapStackIndex)
	                   .arg(info.originalItemId).arg(stack ? stack->index() : -1));
	debugDumpInstances("swapToNew:after", info.instanceTitle);
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
	if (idx < swapStackIndex) {
		DebugDialog::debug(QString("[migration-dbg]   canUndoOwnSwap: undoIdx=%1 < swapStackIndex=%2 (already undone past) -> false")
		                   .arg(idx).arg(swapStackIndex));
		return false;   // user already undid past our swap
	}
	for (int i = swapStackIndex; i < idx; ++i) {
		const QUndoCommand* cmd = stack->command(i);
		bool isSel = dynamic_cast<const SelectItemCommand*>(cmd) != nullptr;
		DebugDialog::debug(QString("[migration-dbg]   canUndoOwnSwap: cmd[%1]='%2' isSelectItem=%3")
		                   .arg(i).arg(cmd ? cmd->text() : QString("<null>"), isSel ? "yes" : "no"));
		if (!isSel) return false;
	}
	return true;
}

void MigrationHandler::revertToOld(MigrationInfo& info)
{
	WaitPushUndoStack* stack = m_sketchWidget->undoStack();

	bool canUndo = canUndoOwnSwap(info.swapStackIndex);
	DebugDialog::debug(QString("[migration-dbg] revertToOld '%1' new=%2 -> old=%3 itemId=%4 swapStackIndex=%5 undoIdx=%6 undoCount=%7 canUndoOwnSwap=%8")
	                   .arg(info.instanceTitle, info.newModuleID, info.oldModuleID)
	                   .arg(info.itemId).arg(info.swapStackIndex)
	                   .arg(stack ? stack->index() : -1).arg(stack ? stack->count() : -1)
	                   .arg(canUndo ? "yes" : "no"));
	debugDumpInstances("revertToOld:before", info.instanceTitle);

	if (canUndo && stack != nullptr) {
		// Undo any selection-only commands stacked on top of our swap, then the swap itself.
		int steps = stack->index() - info.swapStackIndex + 1;
		DebugDialog::debug(QString("[migration-dbg]   revertToOld: CLEAN UNDO x%1, restoring originalItemId=%2").arg(steps).arg(info.originalItemId));
		for (int i = 0; i < steps; ++i) stack->undo();
		info.itemId = info.originalItemId;   // undo restores the original old item (same id)
	}
	else {
		// Real commands intervened (or the swap is gone): don't rewind history. Swap the part
		// forward from new back to old as a fresh, independent command.
		MainWindow* mainWindow = findMainWindow();
		ItemBase* item = currentItem();
		DebugDialog::debug(QString("[migration-dbg]   revertToOld: FORWARD SWAP. currentItem=%1 module=%2 view=%3")
		                   .arg(item ? item->id() : -1)
		                   .arg(item ? item->moduleID() : QString("<null>"))
		                   .arg(item ? int(item->viewID()) : -1));
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

	DebugDialog::debug(QString("[migration-dbg] revertToOld done '%1' itemId=%2 undoIdx=%3").arg(info.instanceTitle).arg(info.itemId).arg(stack ? stack->index() : -1));
	debugDumpInstances("revertToOld:after", info.instanceTitle);
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

void MigrationHandler::goToPreviousMigration()
{
	// Navigation never reverts; the current part keeps whichever version is showing. The only
	// way to switch a part back to old is the version toggle (or Skip/Silence).
	if (m_currentIndex <= 0) return;
	m_currentIndex--;
	updateDialogForCurrentMigration();
}

void MigrationHandler::goToNextMigration()
{
	if (m_currentIndex >= m_pendingMigrations.size() - 1) return;
	m_currentIndex++;
	updateDialogForCurrentMigration();
}

void MigrationHandler::updateAllMigrations()
{
	MainWindow* mainWindow = findMainWindow();
	if (mainWindow == nullptr) { closeDialog(); return; }

	// Bring every part to its new version: swap the ones still showing old (toggled back, or
	// never visited) in a single undoable command. Parts already on the new version are left as-is.
	QList<ItemBase*> toSwap;
	for (int i = 0; i < m_pendingMigrations.size(); ++i) {
		ItemBase* item = currentItem(i);
		if (item != nullptr && item->isObsolete()) toSwap << item;
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
	// The dialog closed (ESC, window close, "Update all", or normal completion). Closing does NOT
	// revert anything — whichever version each part is showing is the user's choice and is kept.
	// Just drop the session state. The dialog deletes itself (WA_DeleteOnClose) and m_dialog
	// (a QPointer) auto-nulls, so a later open starts clean.
	m_pendingMigrations.clear();
	m_currentIndex = 0;

	// These widgets are children of the dialog and are being destroyed with it; drop the
	// dangling raw pointers so nothing touches them before the next createMigrationDialog().
	m_counterLabel = nullptr;
	m_titleLabel = nullptr;
	m_reasonLabel = nullptr;
	m_historyLabel = nullptr;
	m_versionGroup = nullptr;
	m_oldRadio = nullptr;
	m_newRadio = nullptr;
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
