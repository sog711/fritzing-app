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

#include "bugannotation.h"
#include "itembase.h"

#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>

namespace {

class BugItem : public QGraphicsSvgItem {
public:
    using QGraphicsSvgItem::QGraphicsSvgItem;
protected:
    void mousePressEvent(QGraphicsSceneMouseEvent *) override {
        if (scene()) scene()->clearSelection();
        if (parentItem()) parentItem()->setSelected(true);
    }
};

} // namespace

BugAnnotation::BugAnnotation(ItemBase * owner)
	: m_owner(owner)
{
}

BugAnnotation::~BugAnnotation()
{
	// m_item is a QGraphicsItem child of m_owner and will be deleted by Qt
	// when the owner is destroyed. Set to nullptr to prevent double-free
	// if destroyItem() was already called.
	m_item = nullptr;
}

void BugAnnotation::show(const QString & source, const QStringList & errors)
{
	m_errors[source] = errors;

	if (m_item == nullptr) {
		m_item = new BugItem();
		m_item->setAcceptHoverEvents(true);
		m_item->setAcceptedMouseButtons(Qt::LeftButton);
		m_item->setSharedRenderer(&renderer());
		m_item->setZValue(99999);
		m_item->setParentItem(m_owner);
		m_item->setVisible(true);
	}
	QRectF ob = m_owner->boundingRect();
	QRectF ib = m_item->boundingRect();
	m_item->setPos(ob.left() - ib.width(), ob.bottom() - ib.height() / 2);
	updateTooltip();
	m_owner->update();
}

void BugAnnotation::clear(const QString & source)
{
	m_errors.remove(source);
	if (m_errors.isEmpty()) {
		destroyItem();
	} else {
		updateTooltip();
	}
	m_owner->update();
}

bool BugAnnotation::isActive() const
{
	return m_item != nullptr;
}

void BugAnnotation::updateTooltip()
{
	QStringList all;
	for (const QStringList & errs : m_errors) all << errs;
	QString title = m_owner->instanceTitle();
	QString tip = title.isEmpty()
		? all.join(QStringLiteral("\n"))
		: title + QStringLiteral(": ") + all.join(QStringLiteral("\n"));
	m_item->setToolTip(tip);
}

void BugAnnotation::destroyItem()
{
	delete m_item;
	m_item = nullptr;
}

QSvgRenderer & BugAnnotation::renderer()
{
	static QSvgRenderer r;
	if (!r.isValid())
		r.load(QString(":resources/images/part_bug.svg"));
	return r;
}
