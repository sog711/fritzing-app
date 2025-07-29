/*******************************************************************

Part of the Fritzing project - http://fritzing.org
Copyright (c) 2025 Fritzing GmbH

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

#include "FProbeCurrentSketchXml.h"

#include <QByteArray>
#include <QXmlStreamWriter>

FProbeCurrentSketchXml::FProbeCurrentSketchXml(SketchModel * sketchModel) : FProbe("CurrentSketchXml"), m_sketchModel(sketchModel) {
}

FProbeCurrentSketchXml::~FProbeCurrentSketchXml() {
}

QVariant FProbeCurrentSketchXml::read() {
	if (m_sketchModel == nullptr) {
		return QVariant("Error: SketchModel is null");
	}

	try {
		// Create a QByteArray to capture the XML output
		QByteArray xmlData;
		QXmlStreamWriter streamWriter(&xmlData);
		
		// Use the same method as ModelBase::save() to generate XML
		m_sketchModel->save("", streamWriter, false);
		
		// Convert to string and return
		QString xmlString = QString::fromUtf8(xmlData);
		return QVariant(xmlString);
		
	} catch (...) {
		return QVariant("Error: Failed to generate sketch XML");
	}
}