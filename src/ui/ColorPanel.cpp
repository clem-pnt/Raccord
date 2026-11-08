#include "ColorPanel.h"
#include "EffectControls.h"
#include "Widgets.h"
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace rc {

ColorPanel::ColorPanel(QWidget *parent) : QWidget(parent)
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    m_header = new QLabel(this);
    m_header->setContentsMargins(8, 4, 8, 4);
    m_header->setStyleSheet("background: #232328;");
    lay->addWidget(m_header);
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    lay->addWidget(m_scroll, 1);
    Session *s = Session::instance();
    connect(s, &Session::selectionChanged, this, &ColorPanel::rebuild);
    connect(&s->doc(), &Document::changed, this, &ColorPanel::rebuild);
    connect(s, &Session::playheadChanged, this, &ColorPanel::refresh);
    rebuild();
}

void ColorPanel::refresh()
{
    for (auto &f : m_refreshers)
        if (f) f();
}

void ColorPanel::rebuild()
{
    Session *s = Session::instance();
    const Sequence *seq = s->sequence();
    const Clip *clip = nullptr;
    if (seq) {
        for (const auto &id : s->selection()) {
            auto r = seq->findClip(id);
            if (r && r->type == TrackType::Video) {
                clip = seq->clip(id);
                if (id == s->primaryClip()) break;
            }
        }
    }
    const EffectInstance *fx = clip ? clip->effect("lumetri") : nullptr;
    const QString key = clip ? clip->id + (fx ? fx->uid : QString()) : QString();
    if (key == m_key && m_scroll->widget()) {
        refresh();
        return;
    }
    m_key = key;
    m_refreshers.clear();
    auto *body = new QWidget;
    auto *v = new QVBoxLayout(body);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    if (!clip) {
        m_header->setText(tr("Select a video clip to grade it"));
    } else if (!fx) {
        m_header->setText(clip->name);
        auto *b = new QPushButton(tr("Apply Lumetri Color"), body);
        const QString cid = clip->id;
        connect(b, &QPushButton::clicked, this, [s, cid] {
            s->doc().editSequence(tr("Apply Lumetri Color"), [&](Sequence &sq) {
                Clip *c = sq.clip(cid);
                if (!c) return false;
                c->effects.push_back(EffectInstance::create("lumetri"));
                return true;
            });
        });
        v->addWidget(b);
    } else {
        m_header->setText(tr("Lumetri Color: %1").arg(clip->name));
        const EffectDef *def = EffectRegistry::instance().find("lumetri");
        const EffectRef ref{clip->id, fx->uid, false};
        QString group;
        QWidget *content = nullptr;
        QFormLayout *form = nullptr;
        QGridLayout *wheels = nullptr;
        int wheelCol = 0;
        auto flush = [&] {
            if (content) v->addWidget(new Section(trFx(group), content, body, group == "Basic Correction"));
            content = nullptr;
        };
        for (const ParamDef &p : def->params) {
            if (p.group != group || !content) {
                flush();
                group = p.group;
                content = new QWidget;
                auto *cv = new QVBoxLayout(content);
                cv->setContentsMargins(8, 4, 8, 8);
                form = new QFormLayout;
                form->setLabelAlignment(Qt::AlignRight);
                cv->addLayout(form);
                wheels = nullptr;
                wheelCol = 0;
                if (group == "Color Wheels") {
                    wheels = new QGridLayout;
                    cv->addLayout(wheels);
                }
            }
            if (p.type == ParamType::Point && p.id.startsWith("wheel") && wheels) {
                auto *col = new QVBoxLayout;
                auto *lbl = new QLabel(trFx(p.name));
                lbl->setAlignment(Qt::AlignCenter);
                auto *w = new ColorWheel;
                col->addWidget(lbl);
                col->addWidget(w);
                wheels->addLayout(col, 0, wheelCol++);
                const QString pid = p.id;
                connect(w, &ColorWheel::pointChanged, w, [ref, pid](QPointF pt) { params::set(ref, pid, Value(pt.x(), pt.y()), tr("Color Wheel")); });
                connect(w, &ColorWheel::editingFinished, w, [] { params::finish(); });
                m_refreshers.push_back([w, ref, pid] {
                    const Sequence *sq = Session::instance()->sequence();
                    const EffectInstance *e = sq ? params::find(*sq, ref) : nullptr;
                    if (e) w->setPoint(e->value(pid, params::localTime(*sq, ref)).pt());
                });
                continue;
            }
            std::function<void()> refresh;
            QWidget *ed = params::editor(ref, p, refresh, content);
            m_refreshers.push_back(refresh);
            if (p.type == ParamType::Curve) {
                auto *lbl = new QLabel("<b>" + trFx(p.name) + "</b>");
                form->addRow(lbl);
                form->addRow(ed);
            } else if (wheels && p.id.endsWith("Luma")) {
                // Level slider under its wheel.
                wheels->addWidget(ed, 1, wheelCol - 1);
            } else {
                form->addRow(trFx(p.name), ed);
            }
        }
        flush();
    }
    v->addStretch();
    m_scroll->setWidget(body);
    refresh();
}

} // namespace rc
