#ifndef FPSMONITOR_H
#define FPSMONITOR_H

#include <QElapsedTimer>
#include <QVector>
#include <QPainter>

class FPSMonitor {
public:
	FPSMonitor();

	void update();
	void reset();
	qreal getLastFrameFPS() const;
	qreal getMedianFPS() const;
	void printTotalFrameStatistics() const;

	void paint(QPainter* painter, const QRectF& rect, const QWidget* viewport);
	void setShowFPS(bool show);
	bool isShowingFPS() const;

private:
	QElapsedTimer totalTimeTimer;
	QElapsedTimer frameTimer;
	QVector<qreal> renderTimes;
	qint64 totalFrameCount;
	qreal lastFrameFPS;
	bool showFPS;

	qreal calculateMedianFPS() const;
};

#endif // FPSMONITOR_H
