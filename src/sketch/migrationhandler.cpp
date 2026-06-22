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

#include <algorithm>

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
	, m_doneButton(nullptr)
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

	// Present the dialog in a deterministic order: group by part type (the obsolete module id),
	// then by instance title within each group. The collection order is otherwise undefined
	// (it follows scene/view iteration), which made navigation and tests order-dependent.
	std::sort(m_pendingMigrations.begin(), m_pendingMigrations.end(),
	          [](const MigrationInfo& a, const MigrationInfo& b) {
		if (a.oldModuleID != b.oldModuleID) return a.oldModuleID < b.oldModuleID;
		return a.instanceTitle < b.instanceTitle;
	});

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

		// Park the dialog at the top-right of the main window. Navigating the dialog centres and
		// zooms each part in the view, so a centred dialog would sit right on top of that part.
		MainWindow* mainWindow = findMainWindow();
		if (mainWindow != nullptr) {
			const int margin = 16;
			const QRect g = mainWindow->geometry();
			m_dialog->move(g.right() - m_dialog->width() - margin, g.top() + margin);
		}
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

	// New session: zoom afresh on the first part shown, and treat it as a fresh focus.
	m_focusPartSize = -1.0;
	m_focusIndex = -1;

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

	// "Done" simply closes the dialog (each part keeps whatever version it is showing). Always
	// available, so there is an obvious way to finish even when Silence is hidden (forced parts).
	m_doneButton = new QPushButton(tr("Done"), m_dialog);
	m_doneButton->setObjectName("migrationDoneButton");
	m_doneButton->setToolTip(tr("Close this dialog; your choices are kept"));

	actionLayout->addStretch();
	actionLayout->addWidget(m_silenceButton);
	actionLayout->addWidget(m_doneButton);
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
	connect(m_doneButton, &QPushButton::clicked, this, &MigrationHandler::closeDialog);
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
	// Expose the raw instance title (the <b>…</b> markup makes the label text awkward to parse)
	// so tests can address the part with the Wire/Part probes by its exact title.
	if (m_dialog != nullptr) m_dialog->setProperty("currentInstanceTitle", info.instanceTitle);

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
	SketchWidget* view = mainWindow->sketchWidgetForView(item->viewID());
	if (view == nullptr) return;

	// Only move/zoom the view when stepping to a DIFFERENT part. Toggling the current part
	// old<->new (same queue position) must not disturb the view at all -- the swap itself keeps the
	// connectors in place (see SketchWidget::computeSwapAlignOffset), and the user shouldn't see the
	// canvas jump just because they compared the two versions.
	if (m_currentIndex != m_focusIndex) {
		// Bring the view that actually contains this part to the front so the user sees it.
		mainWindow->setCurrentView(item->viewID());

		QRectF bounds = item->sceneBoundingRect();
		if (bounds.isEmpty()) bounds = QRectF(item->pos(), QSizeF(1, 1));

		// Zoom once (the first part shown), then keep that zoom while navigating -- only re-zoom
		// when the next/previous part differs in size by >= 2x (or <= 0.5x) so very different parts
		// still fit. Otherwise just recentre (pan) on the part, which is far less jarring.
		qreal partSize = qMax(bounds.width(), bounds.height());
		bool rezoom = (m_focusPartSize <= 0.0)
		           || (partSize >= m_focusPartSize * 2.0)
		           || (partSize <= m_focusPartSize * 0.5);
		if (rezoom) {
			// Fit the part's bounds plus a margin proportional to its size, so small and large parts
			// both fill a consistent fraction of the view.
			qreal margin = partSize * 2.5;
			view->fitInView(bounds.adjusted(-margin, -margin, margin, margin), Qt::KeepAspectRatio);
			view->updateZoomFromCurrentTransform();
			m_focusPartSize = partSize;
		}
		else {
			view->centerOn(bounds.center());
		}
		m_focusIndex = m_currentIndex;
	}

	view->scene()->clearSelection();
	item->setSelected(true);
}

void MigrationHandler::onVersionToggled(bool useNew)
{
	// Driven by the "new" radio. This is the only place a part is swapped/reverted, so navigating
	// between parts never changes anything until the user actually flips the toggle here.
	if (m_currentIndex < 0 || m_currentIndex >= m_pendingMigrations.size()) return;

	MigrationInfo& info = m_pendingMigrations[m_currentIndex];
	if (useNew && !info.isSwapped) swapToNew(info);
	else if (!useNew && info.isSwapped) revertToOld(info);
	info.decided = true;

	updateDialogForCurrentMigration();
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

	WaitPushUndoStack* stack = m_sketchWidget->undoStack();

	// Remember the old item (its chief id) so a clean undo can restore exactly it.
	info.originalItemId = item->layerKinChief()->id();

	// Reuse the obsolete-swap machinery (swapSelectedAuxAux + portObsoleteSpecialProps). It pushes
	// a single undoable command and returns the new item's id directly, so we never have to
	// rediscover it from the (non-deterministically ordered) selection.
	long newID = mainWindow->swapPartForMigration(item, info.newModuleID);
	if (newID != 0) info.itemId = newID;

	// One top-level command was pushed; remember where, so we can tell later whether our swap
	// is still safely undoable or whether real commands have been pushed on top of it.
	info.swapStackIndex = stack ? stack->index() : -1;
	info.isSwapped = true;

	// The replacement part's *default* title was captured when the migration was queued, but that
	// can misrepresent the migrated instance: the modern resistor defaults to "220 Ω Resistor",
	// yet porting keeps the old 330k. Re-read the actual swapped-in part's title (which reflects the
	// ported resistance/colour) so the "Use new" radio and the change-notes line show the real
	// post-migration value, not the replacement's default.
	if (newID != 0) {
		ItemBase* newItem = mainWindow->findItemInAnyView(newID);
		if (newItem != nullptr && newItem->modelPart() != nullptr) {
			QString actualTitle = newItem->modelPart()->title();
			if (!actualTitle.isEmpty()) info.newTitle = actualTitle;
		}
	}
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
		return false;   // user already undid past our swap
	}
	for (int i = swapStackIndex; i < idx; ++i) {
		const QUndoCommand* cmd = stack->command(i);
		if (dynamic_cast<const SelectItemCommand*>(cmd) == nullptr) return false;
	}
	return true;
}

void MigrationHandler::revertToOld(MigrationInfo& info)
{
	WaitPushUndoStack* stack = m_sketchWidget->undoStack();

	bool canUndo = canUndoOwnSwap(info.swapStackIndex);

	if (canUndo && stack != nullptr) {
		// Undo any selection-only commands stacked on top of our swap, then the swap itself.
		int steps = stack->index() - info.swapStackIndex + 1;
		for (int i = 0; i < steps; ++i) stack->undo();
		info.itemId = info.originalItemId;   // undo restores the original old item (same id)
	}
	else {
		// Our swap isn't on top (later parts' preview swaps sit above it), so we can't simply undo it.
		// Swap the part forward from new back to old as a fresh, independent command.
		MainWindow* mainWindow = findMainWindow();
		ItemBase* item = currentItem();
		if (item != nullptr) item = item->layerKinChief();
		if (mainWindow != nullptr && item != nullptr) {
			long newID = mainWindow->swapPartForMigration(item, info.oldModuleID);
			if (newID != 0) info.itemId = newID;
		}
	}

	info.swapStackIndex = -1;
	info.isSwapped = false;
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

	// The "outdated part" badge should disappear now this instance is silenced; refresh it in
	// every view that holds the part.
	MainWindow* mainWindow = findMainWindow();
	if (mainWindow != nullptr) {
		const ViewLayer::ViewID viewIDs[] = { ViewLayer::BreadboardView, ViewLayer::SchematicView, ViewLayer::PCBView };
		for (ViewLayer::ViewID viewID : viewIDs) {
			SketchWidget* view = mainWindow->sketchWidgetForView(viewID);
			if (view == nullptr) continue;
			ItemBase* viewItem = view->findItem(info.itemId);
			if (viewItem != nullptr) viewItem->layerKinChief()->updateObsoleteAnnotation();
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
	m_focusPartSize = -1.0;
	m_focusIndex = -1;

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
	m_doneButton = nullptr;
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
