#include "fpsmonitor.h"
#include <QDebug>
#include <algorithm>

FPSMonitor::FPSMonitor()
	: totalFrameCount(0)
	, lastFrameFPS(0)
	, showFPS(true)
{
	totalTimeTimer.start();
	frameTimer.start();
}

void FPSMonitor::update()
{
	qreal frameTime = frameTimer.restart() / 1000.0; // Convert to seconds
	if (frameTime > 0) {
		lastFrameFPS = 1.0 / frameTime;
		renderTimes.append(frameTime);
	}
	totalFrameCount++;
}

void FPSMonitor::reset()
{
	totalTimeTimer.restart();
	frameTimer.restart();
	renderTimes.clear();
	totalFrameCount = 0;
	lastFrameFPS = 0;
}

qreal FPSMonitor::getLastFrameFPS() const
{
	return lastFrameFPS;
}

qreal FPSMonitor::getMedianFPS() const
{
	return calculateMedianFPS();
}

qreal FPSMonitor::calculateMedianFPS() const
{
	if (renderTimes.isEmpty()) {
		return 0;
	}

	QVector<qreal> sortedTimes = renderTimes;
	std::sort(sortedTimes.begin(), sortedTimes.end());

	size_t size = sortedTimes.size();
	qreal medianTime;
	if (size % 2 == 0) {
		medianTime = (sortedTimes[size / 2 - 1] + sortedTimes[size / 2]) / 2;
	} else {
		medianTime = sortedTimes[size / 2];
	}

	return medianTime > 0 ? 1.0 / medianTime : 0;
}

void FPSMonitor::printTotalFrameStatistics() const
{
	qint64 totalTimeElapsed = totalTimeTimer.elapsed();
	qDebug() << "Total frames:" << totalFrameCount
			 << "Total time:" << QString::number(totalTimeElapsed / 1000.0, 'f', 2) << "s"
			 << "Overall FPS:"
			 << QString::number(totalFrameCount * 1000.0 / totalTimeElapsed, 'f', 2) << "fps";
}

void FPSMonitor::paint(QPainter* painter, const QRectF& rect, const QWidget* viewport)
{
	if (!showFPS) return;

	painter->save();

	// Reset the painter's transform to work in viewport coordinates
	painter->resetTransform();

	// Set up the font and color for the FPS display
	QFont font = painter->font();
	qreal baseFontSize = 12; // Base font size
	font.setPointSizeF(baseFontSize);
	painter->setFont(font);
	painter->setPen(Qt::white);

	// Create a semi-transparent background for better readability
	QColor bgColor(0, 0, 0, 128);
	painter->setBrush(bgColor);

	// Format the FPS strings
	QString lastFrameFPSString = QString("Last: %1").arg(lastFrameFPS, 0, 'f', 1);
	QString medianFPSString = QString("Median: %1").arg(calculateMedianFPS(), 0, 'f', 1);

	// Calculate the text rectangle
	QFontMetrics fm(font);
	QRect lastFrameRect = fm.boundingRect(lastFrameFPSString);
	QRect medianRect = fm.boundingRect(medianFPSString);
	QRect textRect = lastFrameRect.united(medianRect);
	textRect.adjust(-5, -2, 5, 2);  // Add some padding

	// Position the text in the top-left corner of the viewport
	textRect.moveTopLeft(QPoint(10, 10));

	// Draw the background rectangle and the text
	painter->drawRoundedRect(textRect, 5, 5);
	painter->drawText(textRect.adjusted(0, 0, 0, -textRect.height()/2), Qt::AlignCenter, lastFrameFPSString);
	painter->drawText(textRect.adjusted(0, textRect.height()/2, 0, 0), Qt::AlignCenter, medianFPSString);

	painter->restore();
}


void FPSMonitor::setShowFPS(bool show)
{
	showFPS = show;
}

bool FPSMonitor::isShowingFPS() const
{
	return showFPS;
}
