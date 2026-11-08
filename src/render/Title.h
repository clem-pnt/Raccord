#pragma once
// Text and graphics ("Essential Graphics") rendering with QPainter.
#include "core/Model.h"
#include <QImage>
#include <QJsonObject>

namespace rc {

class TitleRenderer {
public:
    // Returns a cache key that changes only when the rendered image changes.
    static QString key(const QJsonObject &doc, int w, int h, Ticks local, Ticks duration);
    static QImage render(const QJsonObject &doc, int w, int h, Ticks local, Ticks duration);
    static QImage renderCaption(const CaptionStyle &style, const QString &text, int w, int h, int highlightWord = -1);

    // Built-in templates for the Essential Graphics browser.
    static QStringList templateNames();
    static QJsonObject templateDoc(const QString &name);
    static QJsonObject textDoc(const QString &text);
};

} // namespace rc
