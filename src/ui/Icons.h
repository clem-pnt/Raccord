#pragma once
#include <QIcon>
#include <QString>

class QApplication;

namespace rc {

// Vector icons drawn at runtime (no image assets needed, crisp on HiDPI).
class Icons {
public:
    static QIcon get(const QString &name);
};

void applyTheme(QApplication &app);
QColor accentColor();

} // namespace rc
