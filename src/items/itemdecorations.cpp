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

#include "itemdecorations.h"
#include "itembase.h"
#include "partlabel.h"
#include "../connectors/connectoritem.h"
#include "../debugdialog.h"

#include <QGraphicsColorizeEffect>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QTimer>

#include <algorithm>

// keeps the lock symbol (and the sticky symbol chained to its right) off the part's corner
static constexpr double LockSymbolInset = 1.5;

static QSvgRenderer & lockRenderer(bool locked)
{
	static QSvgRenderer closed;
	static QSvgRenderer open;
	QSvgRenderer & renderer = locked ? closed : open;
	if (!renderer.isValid()) {
		QString fn(locked ? ":resources/images/part_lock.svg" : ":resources/images/part_lock_open.svg");
		bool success = renderer.load(fn);
		DebugDialog::debug(QString("movelock load success %1").arg(static_cast<int>(success)));
	}
	return renderer;
}

static QSvgRenderer & stickyRenderer()
{
	static QSvgRenderer renderer;
	if (!renderer.isValid()) {
		(void) renderer.load(QString(":resources/images/part_sticky.svg"));
	}
	return renderer;
}

///////////////////////////////////////////////////

LockSymbolItem::LockSymbolItem(ItemBase * owner)
	: QGraphicsSvgItem(owner),
	  m_owner(owner)
{
	setAcceptedMouseButtons(Qt::LeftButton);
	setCursor(Qt::PointingHandCursor);
}

void LockSymbolItem::setLockedAppearance(bool locked)
{
	setSharedRenderer(&lockRenderer(locked));
}

void LockSymbolItem::setFlashing(bool flashing)
{
	if (flashing == m_flashing) return;

	m_flashing = flashing;
	if (flashing) {
		m_basePos = pos();
		m_baseScale = scale();
		m_baseZ = zValue();
		// a child item can never rise above its parent's z-plane (the part's other layer
		// kin and overlapping parts would hide it), so float on top of the scene while
		// flashing; anchor so the doubled symbol keeps the resting symbol's center
		QPointF sceneTopLeft = m_owner->mapToScene(m_basePos);
		QSizeF half = boundingRect().size() * m_baseScale / 2;
		setParentItem(nullptr);
		setPos(sceneTopLeft - QPointF(half.width(), half.height()));
		setScale(m_baseScale * 2);
		setZValue(999999);
		auto * colorize = new QGraphicsColorizeEffect();
		colorize->setColor(QColor(221, 0, 0));
		colorize->setStrength(1.0);
		setGraphicsEffect(colorize);		// deletes any previous effect
	}
	else {
		setGraphicsEffect(nullptr);
		setParentItem(m_owner);
		setPos(m_basePos);
		setScale(m_baseScale);
		setZValue(m_baseZ);
	}
}

void LockSymbolItem::mousePressEvent(QGraphicsSceneMouseEvent *event)
{
	// no base call: clicking the symbol must not select, move, or deselect anything
	event->accept();
}

void LockSymbolItem::mouseReleaseEvent(QGraphicsSceneMouseEvent *event)
{
	event->accept();
}

void LockSymbolItem::mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event)
{
	m_owner->toggleMoveLockFromSymbol();		// may schedule this symbol for deletion
	event->accept();
	// do not touch any member of this after the toggle
}

///////////////////////////////////////////////////

ItemDecorations::ItemDecorations(ItemBase * owner)
	: m_owner(owner)
{
}

ItemDecorations::~ItemDecorations()
{
	if (m_lockItem) {
		// delete explicitly: during a flash the symbol floats on the scene and would
		// not be cleaned up as a child of the owner
		delete m_lockItem.data();
	}
	// m_stickyItem is a child of the owner and is deleted by Qt with the owner
	m_stickyItem = nullptr;
}

void ItemDecorations::updateLockSymbol()
{
	bool show = m_owner->moveLock() || m_owner->lockSymbolAlwaysVisible();
	if (!show) {
		if (m_lockItem != nullptr) {
			m_lockItem->hide();
			// deleteLater: this call may originate from the symbol's own double-click handler
			m_lockItem->deleteLater();
			m_lockItem = nullptr;
		}
	}
	else {
		if (m_lockItem == nullptr) {
			m_lockItem = new LockSymbolItem(m_owner);
		}
		m_lockItem->setFlashing(false);
		m_lockItem->setLockedAppearance(m_owner->moveLock());
		m_lockItem->setScale(1.0);
		// boards draw the symbol above their opaque graphic; other parts keep the
		// legacy placement behind the part
		m_lockItem->setZValue(m_owner->lockSymbolAlwaysVisible() ? 99999 : -99999);
		m_lockItem->setPos(lockSymbolPosition());
		m_lockItem->setToolTip(m_owner->moveLock()
			? tr("Locked. The part cannot be moved or selected. Double-click to unlock.")
			: tr("Double-click to lock the board in place."));
		m_lockItem->setVisible(true);
	}

	layoutStickySymbol();
	m_owner->update();
}

void ItemDecorations::flashLockSymbol()
{
	if (!m_owner->moveLock()) return;

	updateLockSymbol();
	if (m_lockItem == nullptr) return;

	if (m_flashTimer == nullptr) {
		m_flashTimer = new QTimer(m_owner);
		m_flashTimer->setSingleShot(true);
		m_flashTimer->setInterval(1200);
		QObject::connect(m_flashTimer, &QTimer::timeout, m_owner, [this]() {
			if (m_lockItem) m_lockItem->setFlashing(false);
		});
	}

	m_lockItem->setFlashing(true);
	m_flashTimer->start();
}

void ItemDecorations::setStickyVisible(bool visible)
{
	if (visible) {
		if (m_stickyItem == nullptr) {
			m_stickyItem = new QGraphicsSvgItem();
			m_stickyItem->setAcceptHoverEvents(false);
			m_stickyItem->setAcceptedMouseButtons(Qt::NoButton);
			m_stickyItem->setSharedRenderer(&stickyRenderer());
			m_stickyItem->setZValue(-99999);
			m_stickyItem->setParentItem(m_owner);
			m_stickyItem->setVisible(true);
			layoutStickySymbol();
		}
	}
	else {
		if (m_stickyItem != nullptr) {
			delete m_stickyItem;
			m_stickyItem = nullptr;
		}
	}
}

// Connector pads draw in layers above the symbol and would hide it. Try the home corner
// first, then the four sides of the part label (whether or not the label is shown),
// preferring the side that points toward the part center; if everything collides, give up
// and use the home corner anyway.
QPointF ItemDecorations::lockSymbolPosition() const
{
	QPointF home(LockSymbolInset, LockSymbolInset);
	if (m_lockItem == nullptr || m_owner->scene() == nullptr) return home;

	QSizeF size = m_lockItem->boundingRect().size();
	if (!collidesWithConnectors(QRectF(home, size))) return home;

	PartLabel * label = m_owner->partLabel();
	if (label == nullptr || !label->initialized()) return home;

	QRectF labelRect = m_owner->mapRectFromScene(label->sceneBoundingRect());
	QPointF toCenter = m_owner->boundingRect().center() - labelRect.center();

	QList<QPair<double, QPointF>> candidates;
	candidates.append({ -toCenter.x(), QPointF(labelRect.left() - LockSymbolInset - size.width(), labelRect.center().y() - size.height() / 2) });
	candidates.append({  toCenter.x(), QPointF(labelRect.right() + LockSymbolInset, labelRect.center().y() - size.height() / 2) });
	candidates.append({ -toCenter.y(), QPointF(labelRect.center().x() - size.width() / 2, labelRect.top() - LockSymbolInset - size.height()) });
	candidates.append({  toCenter.y(), QPointF(labelRect.center().x() - size.width() / 2, labelRect.bottom() + LockSymbolInset) });
	std::sort(candidates.begin(), candidates.end(),
	          [](const QPair<double, QPointF> & a, const QPair<double, QPointF> & b) { return a.first > b.first; });

	for (const QPair<double, QPointF> & candidate : candidates) {
		if (!collidesWithConnectors(QRectF(candidate.second, size))) {
			return candidate.second;
		}
	}

	return home;
}

bool ItemDecorations::collidesWithConnectors(const QRectF & ownerRect) const
{
	const QList<QGraphicsItem *> itemsInRect = m_owner->scene()->items(m_owner->mapRectToScene(ownerRect));
	for (QGraphicsItem * gitem : itemsInRect) {
		auto * connectorItem = dynamic_cast<ConnectorItem *>(gitem);
		if (connectorItem && connectorItem->isVisible()) return true;
	}
	return false;
}

void ItemDecorations::layoutStickySymbol()
{
	if (m_stickyItem == nullptr) return;

	m_stickyItem->setPos(m_lockItem == nullptr ? 0 : LockSymbolInset + m_lockItem->boundingRect().width() + 1,
	                     m_lockItem == nullptr ? 0 : LockSymbolInset);
}
