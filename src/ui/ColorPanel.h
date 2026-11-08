#pragma once
// Lumetri Color panel: grading UI bound to the Lumetri effect of the selected clip.
#include "Session.h"
#include <QWidget>
#include <functional>

class QScrollArea;
class QLabel;

namespace rc {

class ColorPanel : public QWidget {
    Q_OBJECT
public:
    explicit ColorPanel(QWidget *parent = nullptr);
private:
    void rebuild();
    void refresh();
    QScrollArea *m_scroll;
    QLabel *m_header;
    QString m_key;
    std::vector<std::function<void()>> m_refreshers;
};

} // namespace rc
