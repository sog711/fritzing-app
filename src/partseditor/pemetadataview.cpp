/*******************************************************************

Part of the Fritzing project - http://fritzing.org
Copyright (c) 2007-2019 Fritzing

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


#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QTextEdit>
#include <QMessageBox>
#include <QtDebug>
#include <QApplication>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QDate>
#include <QColor>
#include <QBrush>
#include <QTimer>
#include <algorithm>

#include "pemetadataview.h"
#include "pehistoryentrydialog.h"
#include "hashpopulatewidget.h"
#include "tageditorwidget.h"

//////////////////////////////////////

FocusOutTextEdit::FocusOutTextEdit(QWidget * parent) : QTextEdit(parent)
{

}

FocusOutTextEdit::~FocusOutTextEdit()
{
}

void FocusOutTextEdit::focusOutEvent(QFocusEvent * e) {
	QTextEdit::focusOutEvent(e);
	if (document()->isModified()) {
		Q_EMIT focusOut();
		document()->setModified(false);
	}
}

//////////////////////////////////////

static bool isHistoryEntryEditable(const HistoryEntry & e) {
	// Only entries from the last three months can be edited/deleted; older ones are read-only.
	QDate d = e.parsedDate();
	return d.isValid() && d >= QDate::currentDate().addMonths(-3);
}

// Display copy of the part's <history> (PEMainWindow::readHistory is the authoritative write path).
// Parses the flat <history> children; for a legacy part with <date>/<author> but no history yet,
// synthesizes a single oldest entry. Returns oldest -> newest.
static QList<HistoryEntry> readHistoryFromDom(const QDomElement & root) {
	QList<HistoryEntry> history;
	for (QDomElement h = root.firstChildElement("history"); !h.isNull(); h = h.nextSiblingElement("history")) {
		HistoryEntry entry;
		entry.date = h.attribute("date");
		entry.author = h.attribute("author");
		entry.mode = h.attribute("mode", "optional");
		entry.description = h.text().trimmed();
		history.append(entry);
	}
	if (history.isEmpty()) {
		QString d = root.firstChildElement("date").text().trimmed();
		QString a = root.firstChildElement("author").text().trimmed();
		if (!d.isEmpty() || !a.isEmpty()) {
			HistoryEntry entry;
			entry.date = d;
			entry.author = a;
			entry.mode = "optional";
			history.append(entry);
		}
	}
	std::stable_sort(history.begin(), history.end(), [](const HistoryEntry & a, const HistoryEntry & b) {
		return a.parsedDate() < b.parsedDate();
	});
	return history;
}

PEMetadataView::PEMetadataView(ReferenceModel * referenceModel, QWidget * parent) : QScrollArea(parent)
{
	m_mainFrame = nullptr;
	m_referenceModel = referenceModel;
	this->setWidgetResizable(true);
	this->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
}

PEMetadataView::~PEMetadataView() {

}

void PEMetadataView::titleEntry() {
	if (m_titleEdit->isModified()) {
		Q_EMIT metadataChanged("title", m_titleEdit->text());
		m_titleEdit->setModified(false);
	}
}

void PEMetadataView::descriptionEntry() {
	if (m_descriptionEdit->document()->isModified()) {
		Q_EMIT metadataChanged("description", m_descriptionEdit->toHtml());
		m_descriptionEdit->document()->setModified(false);
	}
}

void PEMetadataView::urlEntry() {
	if (m_urlEdit->isModified()) {
		Q_EMIT metadataChanged("url", m_urlEdit->text());
		m_urlEdit->setModified(false);
	}
}

void PEMetadataView::labelEntry() {
	if (m_labelEdit->isModified()) {
		Q_EMIT metadataChanged("label", m_labelEdit->text());
		m_labelEdit->setModified(false);
	}
}

void PEMetadataView::familyEntry() {
	if (m_familyEdit->isModified()) {
		Q_EMIT metadataChanged("family", m_familyEdit->text());
		m_familyEdit->setModified(false);
	}
}

void PEMetadataView::variantEntry() {
	if (m_variantEdit->isModified()) {
		Q_EMIT metadataChanged("variant", m_variantEdit->text());
		m_variantEdit->setModified(false);
	}
}

void PEMetadataView::propertiesEntry() {
	Q_EMIT propertiesChanged(m_propertiesEdit->hash());
}

const QHash<QString, QString> & PEMetadataView::properties() {
	return m_propertiesEdit->hash();
}

void PEMetadataView::tagsEntry() {
	Q_EMIT tagsChanged(m_tagsEdit->tags());
}

void PEMetadataView::initMetadata(const QDomDocument & doc)
{
	QWidget * widget = QApplication::focusWidget();
	if (widget && m_mainFrame) {
		QList<QWidget *> children = m_mainFrame->findChildren<QWidget *>();
		if (children.contains(widget)) {
			widget->blockSignals(true);
		}
	}

	if (m_mainFrame) {
		this->setWidget(nullptr);
		delete m_mainFrame;
		m_mainFrame = nullptr;
	}

	QDomElement root = doc.documentElement();
	QDomElement label = root.firstChildElement("label");
	QDomElement author = root.firstChildElement("author");
	QDomElement descr = root.firstChildElement("description");
	QDomElement title = root.firstChildElement("title");
	QDomElement url = root.firstChildElement("url");

	QStringList readOnlyKeys;
	QStringList tagList;            // order-preserving, unlike the old QHash
	QDomElement tags = root.firstChildElement("tags");
	QDomElement tag = tags.firstChildElement("tag");
	while (!tag.isNull()) {
		QString t = tag.text().trimmed();
		if (!t.isEmpty()) tagList << t;
		tag = tag.nextSiblingElement("tag");
	}

	// Autocomplete pool = every tag used across the library. Cached after the first build
	// (initMetadata reruns on every edit/undo, but the pool only needs assembling once).
	if (!m_tagPoolLoaded && m_referenceModel) {
		m_tagPool = m_referenceModel->allTags();
		m_tagPoolLoaded = true;
	}

	QString family;
	QString variant;

	QHash<QString, QString> propertyHash;
	QDomElement properties = root.firstChildElement("properties");
	QDomElement prop = properties.firstChildElement("property");
	while (!prop.isNull()) {
		QString name = prop.attribute("name");
		QString value = prop.text();
		if (name.compare("family", Qt::CaseInsensitive) == 0) {
			family = value;
		}
		else if (name.compare("variant", Qt::CaseInsensitive) == 0) {
			variant = value;
		}
		else {
			propertyHash.insert(name, value);
		}

		prop = prop.nextSiblingElement("property");
	}


	m_mainFrame = new QFrame(this);
	m_mainFrame->setObjectName("metadataMainFrame");
	auto *mainLayout = new QVBoxLayout(m_mainFrame);
	mainLayout->setSizeConstraint( QLayout::SetMinAndMaxSize );

	auto *explanation = new QLabel(tr("This is where you edit the metadata for the part ..."));
	mainLayout->addWidget(explanation);

	auto * formLayout = new QFormLayout();
	auto * formFrame = new QFrame;
	mainLayout->addWidget(formFrame);

	m_titleEdit = new QLineEdit();
	m_titleEdit->setText(title.text());
	connect(m_titleEdit, SIGNAL(editingFinished()), this, SLOT(titleEntry()));
	m_titleEdit->setObjectName("PartsEditorLineEdit");
	m_titleEdit->setStatusTip(tr("Set the part's title"));
	formLayout->addRow(tr("Title"), m_titleEdit);

	// History table replaces the old standalone Date + Author rows: each part revision is a
	// <history> entry, and the newest entry's date/author are mirrored back into the top-level
	// <date>/<author> on write (PEMainWindow::writeHistory).
	m_history = readHistoryFromDom(root);
	m_defaultAuthor = author.text().trimmed();

	m_historyTable = new QTableWidget();
	m_historyTable->setObjectName("PartsEditorHistoryTable");
	m_historyTable->setColumnCount(5);
	m_historyTable->setHorizontalHeaderLabels(QStringList() << tr("Date") << tr("Author") << tr("Changes") << tr("Mode") << QString());
	m_historyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_historyTable->setSelectionMode(QAbstractItemView::NoSelection);
	m_historyTable->verticalHeader()->setVisible(false);
	m_historyTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
	m_historyTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);   // "Changes" takes the slack
	m_historyTable->setStatusTip(tr("The part's revision history. Entries from the last three months can be edited or deleted (double-click / ×); older ones are read-only."));
	connect(m_historyTable, SIGNAL(cellDoubleClicked(int, int)), this, SLOT(historyCellDoubleClicked(int, int)));

	auto * historyContainer = new QWidget();
	auto * historyVLayout = new QVBoxLayout(historyContainer);
	historyVLayout->setContentsMargins(0, 0, 0, 0);
	historyVLayout->addWidget(m_historyTable);
	auto * addHistoryButton = new QPushButton(tr("+"));
	addHistoryButton->setObjectName("PartsEditorHistoryAdd");
	addHistoryButton->setToolTip(tr("Add a revision entry"));
	addHistoryButton->setStatusTip(tr("Add a revision entry"));
	connect(addHistoryButton, SIGNAL(clicked()), this, SLOT(addHistoryEntry()));
	auto * addHistoryRow = new QHBoxLayout();
	addHistoryRow->addWidget(addHistoryButton);
	addHistoryRow->addStretch(1);
	historyVLayout->addLayout(addHistoryRow);
	formLayout->addRow(tr("History"), historyContainer);

	populateHistoryTable();

	m_descriptionEdit = new FocusOutTextEdit();
	m_descriptionEdit->setText(descr.text());
	m_descriptionEdit->document()->setModified(false);
	connect(m_descriptionEdit, SIGNAL(focusOut()), this, SLOT(descriptionEntry()));
	m_descriptionEdit->setObjectName("PartsEditorTextEdit");
	m_descriptionEdit->setStatusTip(tr("Set the part's description--you can use simple html (as defined by Qt's Rich Text)"));
	formLayout->addRow(tr("Description"), m_descriptionEdit);

	m_labelEdit = new QLineEdit();
	m_labelEdit->setText(label.text());
	connect(m_labelEdit, SIGNAL(editingFinished()), this, SLOT(labelEntry()));
	m_labelEdit->setObjectName("PartsEditorLineEdit");
	m_labelEdit->setStatusTip(tr("Set the default part label prefix"));
	formLayout->addRow(tr("Label"), m_labelEdit);

	m_urlEdit = new QLineEdit();
	m_urlEdit->setText(url.text().trimmed());
	connect(m_urlEdit, SIGNAL(editingFinished()), this, SLOT(urlEntry()));
	m_urlEdit->setObjectName("PartsEditorLineEdit");
	m_urlEdit->setStatusTip(tr("Set the part's url if it is described on a web page"));
	formLayout->addRow(tr("URL"), m_urlEdit);

	m_familyEdit = new QLineEdit();
	m_familyEdit->setText(family);
	connect(m_familyEdit, SIGNAL(editingFinished()), this, SLOT(familyEntry()));
	m_familyEdit->setObjectName("PartsEditorLineEdit");
	m_familyEdit->setStatusTip(tr("Set the part's family--what other parts is this part related to"));
	formLayout->addRow(tr("Family"), m_familyEdit);

	m_variantEdit = new QLineEdit();
	m_variantEdit->setText(variant);
	connect(m_variantEdit, SIGNAL(editingFinished()), this, SLOT(variantEntry()));
	m_variantEdit->setObjectName("PartsEditorLineEdit");
	m_variantEdit->setStatusTip(tr("Set the part's variant--this makes it unique from all other parts in the same family"));
	formLayout->addRow(tr("Variant"), m_variantEdit);

	m_propertiesEdit = new HashPopulateWidget("", propertyHash, readOnlyKeys, false, this);
	m_propertiesEdit->setObjectName("PartsEditorPropertiesEdit");
	m_propertiesEdit->setStatusTip(tr("Set the part's properties"));
	connect(m_propertiesEdit, SIGNAL(changed()), this, SLOT(propertiesEntry()));
	formLayout->addRow(tr("Properties"), m_propertiesEdit);

	m_tagsEdit = new TagEditorWidget(tagList, m_tagPool, this);
	m_tagsEdit->setObjectName("PartsEditorTagEditor");
	m_tagsEdit->setStatusTip(tr("Set the part's tags"));
	connect(m_tagsEdit, SIGNAL(changed()), this, SLOT(tagsEntry()));
	formLayout->addRow(tr("Tags"), m_tagsEdit);

	formFrame->setLayout(formLayout);
	m_mainFrame->setLayout(mainLayout);

	this->setWidget(m_mainFrame);
}

void PEMetadataView::resetProperty(const QString & name, const QString & value)
{
	if (name == "family") m_familyEdit->setText(value);
	else if (name == "variant") m_variantEdit->setText(value);
}

QString PEMetadataView::family() {
	return m_familyEdit->text();
}

QString PEMetadataView::variant() {
	return m_variantEdit->text();
}

void PEMetadataView::populateHistoryTable() {
	if (m_historyTable == nullptr) return;
	m_historyTable->setRowCount(m_history.count());
	for (int i = 0; i < m_history.count(); ++i) {
		const HistoryEntry & e = m_history.at(i);

		auto * dateItem = new QTableWidgetItem(e.date);
		auto * authorItem = new QTableWidgetItem(e.author);
		QString preview = e.description;
		preview.replace('\n', ' ');                  // single-line preview; full text in the tooltip
		auto * changesItem = new QTableWidgetItem(preview);
		changesItem->setToolTip(e.description);

		QString modeText;
		QColor modeColor;
		if (e.isRequired()) { modeText = tr("required"); modeColor = QColor(0xb5, 0x34, 0x1f); }       // red
		else if (e.isRecommended()) { modeText = tr("recommended"); modeColor = QColor(0x1f, 0x5f, 0xb5); }  // blue
		else { modeText = tr("optional"); modeColor = QColor(0x80, 0x80, 0x80); }                       // grey
		auto * modeItem = new QTableWidgetItem(modeText);
		modeItem->setForeground(modeColor);

		m_historyTable->setItem(i, 0, dateItem);
		m_historyTable->setItem(i, 1, authorItem);
		m_historyTable->setItem(i, 2, changesItem);
		m_historyTable->setItem(i, 3, modeItem);

		if (isHistoryEntryEditable(e)) {
			// recent entries (<= 3 months) get Edit + delete buttons; older ones stay read-only
			auto * actions = new QWidget();
			auto * actionsLayout = new QHBoxLayout(actions);
			actionsLayout->setContentsMargins(0, 0, 0, 0);
			actionsLayout->setSpacing(2);

			auto * editButton = new QPushButton(tr("Edit"));
			editButton->setObjectName("PartsEditorHistoryEdit");
			editButton->setToolTip(tr("Edit this revision"));
			connect(editButton, &QPushButton::clicked, this, [this, i]() { editHistoryRow(i); });

			auto * del = new QPushButton(QString(QChar(ushort(0x00D7))));   // × U+00D7 multiplication sign
			del->setObjectName("PartsEditorHistoryDelete");
			del->setToolTip(tr("Delete this revision"));
			connect(del, &QPushButton::clicked, this, [this, i]() {
				if (i < 0 || i >= m_history.count()) return;
				m_history.removeAt(i);
				commitHistory();
			});

			actionsLayout->addWidget(editButton);
			actionsLayout->addWidget(del);
			m_historyTable->setCellWidget(i, 4, actions);
		}
	}
}

void PEMetadataView::addHistoryEntry() {
	HistoryEntry entry;
	entry.date = QDate::currentDate().toString(Qt::ISODate);
	entry.author = m_defaultAuthor;
	entry.mode = "optional";
	PEHistoryEntryDialog dialog(entry, false, QString(), this);
	dialog.setWindowTitle(tr("Add revision"));
	if (dialog.exec() != QDialog::Accepted) return;
	m_history.append(dialog.entry());
	commitHistory();
}

void PEMetadataView::historyCellDoubleClicked(int row, int column) {
	Q_UNUSED(column);
	editHistoryRow(row);          // bonus affordance; the per-row Edit button is the primary one
}

void PEMetadataView::editHistoryRow(int row) {
	if (row < 0 || row >= m_history.count()) return;
	const HistoryEntry & e = m_history.at(row);
	if (!isHistoryEntryEditable(e)) return;          // older than three months: read-only
	PEHistoryEntryDialog dialog(e, false, QString(), this);
	dialog.setWindowTitle(tr("Edit revision"));
	if (dialog.exec() != QDialog::Accepted) return;
	m_history[row] = dialog.entry();
	commitHistory();
}

void PEMetadataView::commitHistory() {
	// Defer one event-loop tick: applying the change rebuilds this whole metadata frame (via
	// PEMainWindow::changeHistory -> initMetadata), deleting the +/x/table widget whose signal we
	// are still inside -- emitting synchronously would free the sender mid-emission.
	QTimer::singleShot(0, this, [this]() { Q_EMIT historyChanged(m_history); });
}
