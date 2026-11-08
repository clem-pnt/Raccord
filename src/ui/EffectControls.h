#pragma once
// Effect Controls panel: parameters, keyframes and masks of the selected clip
// or transition. Also hosts the shared parameter editing helpers.
#include "Session.h"
#include <QScrollArea>
#include <QWidget>
#include <functional>

class QVBoxLayout;
class QLabel;

namespace rc {

// Locates an effect instance in the active sequence: a clip effect or the
// effect of a transition (uid empty, transition id in clipId).
struct EffectRef {
    QString clipId;
    QString effectUid;
    bool transition = false;
};

// Helpers shared by Effect Controls and the Lumetri panel.
namespace params {
const EffectInstance *find(const Sequence &seq, const EffectRef &r);
EffectInstance *find(Sequence &seq, const EffectRef &r);
// Local time of the playhead inside the clip (keyframes are clip-relative).
Ticks localTime(const Sequence &seq, const EffectRef &r);
// Sets a parameter (adds a key if animated). Consecutive calls with the same
// mergeKey collapse into one undo step until endMerge().
void set(const EffectRef &r, const QString &pid, const Value &v, const QString &label);
void finish();
// Builds an editor widget for one parameter. `refresh` re-reads the value.
QWidget *editor(const EffectRef &r, const ParamDef &def, std::function<void()> &refresh, QWidget *parent);
}

class EffectControls : public QWidget {
    Q_OBJECT
public:
    explicit EffectControls(QWidget *parent = nullptr);

private:
    void rebuild();
    void refreshValues();
    QWidget *effectBlock(const Clip &clip, const EffectInstance &fx, int index, int count);
    QWidget *paramRow(const EffectRef &ref, const ParamDef &def);
    QWidget *maskBlock(const EffectRef &ref, int maskIndex);
    QWidget *transitionBlock(const Transition &t);
    QWidget *titleBlock(const Clip &clip);

    QScrollArea *m_scroll;
    QWidget *m_body = nullptr;
    QLabel *m_header;
    std::vector<std::function<void()>> m_refreshers;
    QString m_shownKey;      // clip/transition id + structure hash
};

} // namespace rc
