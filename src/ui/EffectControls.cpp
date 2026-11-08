#include "EffectControls.h"
#include "Icons.h"
#include "Widgets.h"
#include "render/Title.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace rc {

namespace params {

static Transition *findTransition(Sequence &seq, const QString &id)
{
    for (auto *v : {&seq.videoTracks, &seq.audioTracks})
        for (auto &t : *v)
            if (Transition *tr = t.transition(id))
                return tr;
    return nullptr;
}

const EffectInstance *find(const Sequence &seq, const EffectRef &r)
{
    return find(const_cast<Sequence &>(seq), r);
}

EffectInstance *find(Sequence &seq, const EffectRef &r)
{
    if (r.transition) {
        Transition *t = findTransition(seq, r.clipId);
        return t ? &t->effect : nullptr;
    }
    Clip *c = seq.clip(r.clipId);
    return c ? c->effectByUid(r.effectUid) : nullptr;
}

Ticks localTime(const Sequence &seq, const EffectRef &r)
{
    if (r.transition) {
        for (const auto *v : {&seq.videoTracks, &seq.audioTracks})
            for (const auto &t : *v)
                for (const auto &tr : t.transitions)
                    if (tr.id == r.clipId) {
                        TimeRange rg = edit::transitionRange(t, tr);
                        return std::clamp(seq.playhead - rg.start, Ticks(0), rg.duration());
                    }
        return 0;
    }
    const Clip *c = seq.clip(r.clipId);
    return c ? std::clamp(seq.playhead - c->start, Ticks(0), c->duration) : 0;
}

void set(const EffectRef &r, const QString &pid, const Value &v, const QString &label)
{
    Session *s = Session::instance();
    const QString key = r.clipId + "/" + r.effectUid + "/" + pid;
    const QString seqId = s->project().activeSequenceId;
    s->doc().editMerged(label, key, [&](Project &p) {
        Sequence *seq = p.sequence(seqId);
        if (!seq) return false;
        EffectInstance *fx = find(*seq, r);
        if (!fx) return false;
        fx->param(pid).setValue(v, localTime(*seq, r));
        return true;
    });
}

void finish() { Session::instance()->doc().endMerge(); }

static double pointScale(int axis)
{
    const Sequence *seq = Session::instance()->sequence();
    return seq ? (axis == 0 ? seq->width : seq->height) : 1;
}

static Value current(const EffectRef &r, const QString &pid)
{
    const Sequence *seq = Session::instance()->sequence();
    if (!seq) return {};
    const EffectInstance *fx = find(*seq, r);
    return fx ? fx->value(pid, localTime(*seq, r)) : Value();
}

QWidget *editor(const EffectRef &r, const ParamDef &def, std::function<void()> &refresh, QWidget *parent)
{
    const QString pid = def.id;
    const QString label = QObject::tr("Change %1").arg(trFx(def.name));
    switch (def.type) {
    case ParamType::Float:
    case ParamType::Int:
    case ParamType::Angle: {
        auto *w = new ValueSlider(parent);
        w->setRange(def.min, def.max, def.type != ParamType::Int || def.max - def.min > 1000);
        w->setStep(def.type == ParamType::Int ? 1 : def.step > 0 ? def.step : 0.1);
        w->setDecimals(def.type == ParamType::Int ? 0 : def.step >= 1 ? 0 : def.step >= 0.1 ? 1 : 2);
        w->setSuffix(def.type == ParamType::Angle ? QStringLiteral("°") : def.unit.isEmpty() ? QString() : " " + def.unit);
        w->setShowBar(def.max > def.min);
        QObject::connect(w, &ValueSlider::valueChanged, w, [r, pid, label, def](double v) {
            set(r, pid, Value(def.type == ParamType::Int ? std::round(v) : v), label);
        });
        QObject::connect(w, &ValueSlider::editingFinished, w, [] { finish(); });
        refresh = [w, r, pid] { w->setValue(current(r, pid).f()); };
        return w;
    }
    case ParamType::Bool: {
        auto *w = new QCheckBox(parent);
        QObject::connect(w, &QCheckBox::clicked, w, [r, pid, label](bool on) { set(r, pid, Value(on ? 1.0 : 0.0), label); finish(); });
        refresh = [w, r, pid] { QSignalBlocker b(w); w->setChecked(current(r, pid).b()); };
        return w;
    }
    case ParamType::Enum: {
        auto *w = new QComboBox(parent);
        w->addItems(trFx(def.options));
        QObject::connect(w, QOverload<int>::of(&QComboBox::activated), w, [r, pid, label](int i) { set(r, pid, Value(double(i)), label); finish(); });
        refresh = [w, r, pid] { QSignalBlocker b(w); w->setCurrentIndex(current(r, pid).i()); };
        return w;
    }
    case ParamType::Color: {
        auto *w = new ColorButton(parent);
        QObject::connect(w, &ColorButton::colorChanged, w, [r, pid, label](const QColor &c) { set(r, pid, Value::fromColor(c), label); finish(); });
        refresh = [w, r, pid] { w->setColor(current(r, pid).color()); };
        return w;
    }
    case ParamType::Point: {
        auto *box = new QWidget(parent);
        auto *l = new QHBoxLayout(box);
        l->setContentsMargins(0, 0, 0, 0);
        const bool wheel = pid.startsWith("wheel");
        ValueSlider *xs[2];
        for (int a = 0; a < 2; ++a) {
            auto *s = new ValueSlider(box);
            s->setRange(-1e6, 1e6);
            s->setStep(wheel ? 0.005 : 1);
            s->setDecimals(wheel ? 3 : 1);
            l->addWidget(s);
            xs[a] = s;
        }
        for (int a = 0; a < 2; ++a)
            QObject::connect(xs[a], &ValueSlider::valueChanged, box, [=](double v) {
                Value cur = current(r, pid);
                cur.v[a] = wheel ? v : v / pointScale(a);
                set(r, pid, cur, label);
            }), QObject::connect(xs[a], &ValueSlider::editingFinished, box, [] { finish(); });
        refresh = [=] {
            Value v = current(r, pid);
            xs[0]->setValue(wheel ? v.v[0] : v.v[0] * pointScale(0));
            xs[1]->setValue(wheel ? v.v[1] : v.v[1] * pointScale(1));
        };
        return box;
    }
    case ParamType::String: {
        auto *w = new QLineEdit(parent);
        QObject::connect(w, &QLineEdit::editingFinished, w, [w, r, pid, label] { set(r, pid, Value(w->text()), label); finish(); });
        refresh = [w, r, pid] { if (!w->hasFocus()) w->setText(current(r, pid).s); };
        return w;
    }
    case ParamType::Text: {
        auto *w = new QPlainTextEdit(parent);
        w->setMaximumHeight(70);
        QObject::connect(w, &QPlainTextEdit::textChanged, w, [w, r, pid, label] { set(r, pid, Value(w->toPlainText()), label); });
        refresh = [w, r, pid] {
            if (!w->hasFocus()) { QSignalBlocker b(w); w->setPlainText(current(r, pid).s); }
        };
        return w;
    }
    case ParamType::File: {
        auto *box = new QWidget(parent);
        auto *l = new QHBoxLayout(box);
        l->setContentsMargins(0, 0, 0, 0);
        auto *name = new QLabel(box);
        name->setMinimumWidth(60);
        auto *browse = new QToolButton(box);
        browse->setText("...");
        auto *clear = new QToolButton(box);
        clear->setText("x");
        l->addWidget(name, 1);
        l->addWidget(browse);
        l->addWidget(clear);
        QObject::connect(browse, &QToolButton::clicked, box, [box, r, pid, label] {
            QString f = QFileDialog::getOpenFileName(box, QObject::tr("Choose File"), {}, QObject::tr("LUT (*.cube);;All files (*)"));
            if (!f.isEmpty()) { set(r, pid, Value(f), label); finish(); }
        });
        QObject::connect(clear, &QToolButton::clicked, box, [r, pid, label] { set(r, pid, Value(QString()), label); finish(); });
        refresh = [name, r, pid] {
            QString s = current(r, pid).s;
            name->setText(s.isEmpty() ? QObject::tr("None") : QFileInfo(s).fileName());
            name->setToolTip(s);
        };
        return box;
    }
    case ParamType::Curve: {
        auto *box = new QWidget(parent);
        auto *l = new QVBoxLayout(box);
        l->setContentsMargins(0, 0, 0, 0);
        const bool rgb = pid == "curveRgb";
        auto *ed = new CurveEditor(rgb ? CurveEditor::Rgb : CurveEditor::Hue, box);
        if (rgb) {
            auto *row = new QHBoxLayout;
            auto *grp = new QButtonGroup(box);
            const char *names[4] = {"Luma", "R", "G", "B"};
            for (int i = 0; i < 4; ++i) {
                auto *b = new QToolButton(box);
                b->setText(names[i]);
                b->setCheckable(true);
                b->setChecked(i == 0);
                grp->addButton(b, i);
                row->addWidget(b);
            }
            row->addStretch();
            QObject::connect(grp, &QButtonGroup::idClicked, ed, &CurveEditor::setChannel);
            l->addLayout(row);
        }
        l->addWidget(ed);
        auto *hint = new QLabel(QObject::tr("Click to add, right-click to remove, double-click to reset"), box);
        hint->setStyleSheet("color: #888; font-size: 10px;");
        l->addWidget(hint);
        QObject::connect(ed, &CurveEditor::changed, ed, [ed, r, pid, label] { set(r, pid, Value(ed->json()), label); });
        QObject::connect(ed, &CurveEditor::editingFinished, ed, [] { finish(); });
        refresh = [ed, r, pid] {
            QString j = current(r, pid).s;
            if (j != ed->json()) ed->setJson(j);
        };
        return box;
    }
    }
    return new QWidget(parent);
}

} // namespace params

// ---------------------------------------------------------------------------

EffectControls::EffectControls(QWidget *parent) : QWidget(parent)
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    m_header = new QLabel(this);
    m_header->setContentsMargins(8, 4, 8, 4);
    m_header->setStyleSheet("background: #232328; color: #ccc;");
    lay->addWidget(m_header);
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    lay->addWidget(m_scroll, 1);
    Session *s = Session::instance();
    connect(s, &Session::selectionChanged, this, &EffectControls::rebuild);
    connect(&s->doc(), &Document::changed, this, &EffectControls::rebuild);
    connect(s, &Session::playheadChanged, this, &EffectControls::refreshValues);
    connect(s, &Session::sequenceChanged, this, &EffectControls::rebuild);
    rebuild();
}

static QString structureKey(const Clip &c)
{
    QString k = c.id + "|" + QString::number(int(c.source));
    for (const auto &e : c.effects) {
        k += "|" + e.uid + ":" + QString::number(e.masks.size()) + (e.enabled ? "1" : "0");
        for (const auto &[id, p] : e.params)
            if (p.animated()) k += "a" + id;
    }
    if (c.source == ClipSource::Title)
        k += "|" + QString::number(c.gen["layers"].toArray().size());
    return k;
}

void EffectControls::rebuild()
{
    Session *s = Session::instance();
    const Sequence *seq = s->sequence();
    QString key;
    const Clip *clip = nullptr;
    const Transition *trans = nullptr;
    if (seq) {
        if (!s->selectedTransition().isEmpty()) {
            for (const auto *v : {&seq->videoTracks, &seq->audioTracks})
                for (const auto &t : *v)
                    for (const auto &tr : t.transitions)
                        if (tr.id == s->selectedTransition()) trans = &tr;
            if (trans) key = "T" + trans->id + trans->effect.type;
        } else {
            QString id = s->primaryClip();
            if (id.isEmpty() && !s->selection().isEmpty()) id = *s->selection().begin();
            // Prefer the video clip of a linked pair (Premiere shows both, video first).
            clip = seq->clip(id);
            if (clip) key = "C" + structureKey(*clip);
        }
    }
    if (key == m_shownKey && m_body) {
        refreshValues();
        return;
    }
    m_shownKey = key;
    m_refreshers.clear();
    m_body = new QWidget;
    auto *lay = new QVBoxLayout(m_body);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    if (trans) {
        const EffectDef *d = EffectRegistry::instance().find(trans->effect.type);
        m_header->setText(tr("Transition: %1").arg(d ? trFx(d->name) : trans->effect.type));
        lay->addWidget(transitionBlock(*trans));
    } else if (clip) {
        m_header->setText(tr("Source: %1").arg(clip->name.isEmpty() ? clipSourceName(clip->source) : clip->name));
        // Show every clip in the link group (video then audio).
        std::vector<const Clip *> shown{clip};
        for (const auto &id : seq->linkedClips(clip->id))
            if (id != clip->id)
                if (const Clip *o = seq->clip(id)) shown.push_back(o);
        for (const Clip *c : shown) {
            auto r = seq->findClip(c->id);
            if (shown.size() > 1) {
                auto *lbl = new QLabel(r && r->type == TrackType::Video ? tr("Video") : tr("Audio"));
                lbl->setContentsMargins(6, 6, 6, 2);
                lbl->setStyleSheet("color: #8fb4ff; font-weight: bold;");
                lay->addWidget(lbl);
            }
            if (c->source == ClipSource::Title)
                lay->addWidget(titleBlock(*c));
            for (int i = 0; i < (int)c->effects.size(); ++i)
                lay->addWidget(effectBlock(*c, c->effects[i], i, (int)c->effects.size()));
        }
    } else {
        m_header->setText(tr("(no clip selected)"));
    }
    lay->addStretch();
    m_scroll->setWidget(m_body);
    refreshValues();
}

void EffectControls::refreshValues()
{
    for (auto &f : m_refreshers)
        if (f) f();
}

QWidget *EffectControls::paramRow(const EffectRef &ref, const ParamDef &def)
{
    auto *row = new QWidget;
    auto *l = new QHBoxLayout(row);
    l->setContentsMargins(6, 1, 4, 1);
    l->setSpacing(4);
    Session *s = Session::instance();
    auto *watch = new QToolButton(row);
    watch->setAutoRaise(true);
    watch->setCheckable(true);
    watch->setIcon(Icons::get("stopwatch"));
    watch->setToolTip(tr("Toggle animation"));
    watch->setVisible(def.animatable && def.type != ParamType::String && def.type != ParamType::File && def.type != ParamType::Text);
    l->addWidget(watch);
    auto *name = new QLabel(trFx(def.name), row);
    name->setMinimumWidth(110);
    l->addWidget(name);
    std::function<void()> refresh;
    QWidget *ed = params::editor(ref, def, refresh, row);
    const bool big = def.type == ParamType::Curve;
    if (!big)
        l->addWidget(ed, 1);
    auto *prev = new QToolButton(row), *key = new QToolButton(row), *next = new QToolButton(row), *reset = new QToolButton(row);
    prev->setArrowType(Qt::LeftArrow);
    next->setArrowType(Qt::RightArrow);
    key->setIcon(Icons::get("keyframe"));
    key->setToolTip(tr("Add/Remove Keyframe"));
    reset->setIcon(Icons::get("reset"));
    reset->setToolTip(tr("Reset Parameter"));
    for (auto *b : {prev, key, next, reset}) {
        b->setAutoRaise(true);
        b->setFixedSize(18, 18);
        l->addWidget(b);
    }
    const QString pid = def.id;
    auto animated = [ref, pid]() -> const AnimatedParam * {
        const Sequence *seq = Session::instance()->sequence();
        const EffectInstance *fx = seq ? params::find(*seq, ref) : nullptr;
        if (!fx) return nullptr;
        auto it = fx->params.find(pid);
        return it == fx->params.end() ? nullptr : &it->second;
    };
    auto editParam = [ref, pid](const QString &label, const std::function<void(AnimatedParam &, Ticks)> &fn) {
        Session *s = Session::instance();
        s->doc().editSequence(label, [&](Sequence &seq) {
            EffectInstance *fx = params::find(seq, ref);
            if (!fx) return false;
            fn(fx->param(pid), params::localTime(seq, ref));
            return true;
        });
    };
    connect(watch, &QToolButton::clicked, row, [=](bool on) {
        editParam(on ? tr("Enable Keyframes") : tr("Disable Keyframes"), [on](AnimatedParam &p, Ticks t) { p.setAnimated(on, t); });
    });
    connect(key, &QToolButton::clicked, row, [=] {
        editParam(tr("Toggle Keyframe"), [](AnimatedParam &p, Ticks t) {
            if (!p.animated()) { p.setAnimated(true, t); return; }
            if (p.keyIndexAt(t, 1) >= 0) p.removeKeyAt(t, 1);
            else p.addKey({t, p.valueAt(t), Interp::Linear});
        });
    });
    auto jump = [=](int dir) {
        const AnimatedParam *p = animated();
        const Sequence *seq = Session::instance()->sequence();
        if (!p || !p->animated() || !seq) return;
        const Ticks lt = params::localTime(*seq, ref);
        const Ticks base = seq->playhead - lt;
        Ticks best = kInvalidTicks;
        for (const auto &k : p->keys()) {
            if (dir > 0 && k.time > lt && (best == kInvalidTicks || k.time < best)) best = k.time;
            if (dir < 0 && k.time < lt && (best == kInvalidTicks || k.time > best)) best = k.time;
        }
        if (best != kInvalidTicks) Session::instance()->setPlayhead(base + best);
    };
    connect(prev, &QToolButton::clicked, row, [=] { jump(-1); });
    connect(next, &QToolButton::clicked, row, [=] { jump(1); });
    const Value defv = def.def;
    connect(reset, &QToolButton::clicked, row, [=] {
        editParam(tr("Reset %1").arg(trFx(def.name)), [defv](AnimatedParam &p, Ticks t) { p.setValue(defv, t); });
    });
    // Keyframe context menu on the key button: interpolation.
    key->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(key, &QToolButton::customContextMenuRequested, row, [=](QPoint pos) {
        QMenu m;
        const std::pair<Interp, QString> modes[] = {{Interp::Linear, tr("Linear")}, {Interp::Bezier, tr("Bezier")}, {Interp::Hold, tr("Hold")},
                                                    {Interp::EaseIn, tr("Ease In")}, {Interp::EaseOut, tr("Ease Out")}, {Interp::EaseInOut, tr("Ease In/Out")}};
        for (const auto &[mode, text] : modes) {
            const Interp im = mode;
            m.addAction(text, [=] {
                editParam(tr("Keyframe Interpolation"), [im](AnimatedParam &p, Ticks t) {
                    int i = p.keyIndexAt(t, kTicksPerSecond / 100);
                    if (i < 0) for (auto &k : p.keys()) k.interp = im;
                    else p.keys()[i].interp = im;
                });
            });
        }
        m.exec(key->mapToGlobal(pos));
    });
    m_refreshers.push_back([=] {
        if (refresh) refresh();
        const AnimatedParam *p = animated();
        const bool anim = p && p->animated();
        QSignalBlocker b(watch);
        watch->setChecked(anim);
        prev->setVisible(anim);
        next->setVisible(anim);
        key->setVisible(def.animatable && watch->isVisible());
        const Sequence *seq = Session::instance()->sequence();
        const bool onKey = anim && seq && p->keyIndexAt(params::localTime(*seq, ref), 1) >= 0;
        key->setStyleSheet(onKey ? "QToolButton { background: #4a90ff; border-radius: 3px; }" : QString());
    });
    if (!big)
        return row;
    auto *wrap = new QWidget;
    auto *v = new QVBoxLayout(wrap);
    v->setContentsMargins(0, 0, 0, 4);
    v->addWidget(row);
    ed->setParent(wrap);
    v->addWidget(ed);
    (void)s;
    return wrap;
}

QWidget *EffectControls::maskBlock(const EffectRef &ref, int mi)
{
    auto *w = new QWidget;
    auto *f = new QFormLayout(w);
    f->setContentsMargins(24, 2, 6, 2);
    Session *s = Session::instance();
    auto editMask = [ref, mi](const QString &label, const QString &key, const std::function<void(Mask &, Ticks)> &fn) {
        Session *s = Session::instance();
        const QString seqId = s->project().activeSequenceId;
        s->doc().editMerged(label, key, [&](Project &p) {
            Sequence *seq = p.sequence(seqId);
            EffectInstance *fx = seq ? params::find(*seq, ref) : nullptr;
            if (!fx || mi >= (int)fx->masks.size()) return false;
            fn(fx->masks[mi], params::localTime(*seq, ref));
            return true;
        });
    };
    auto maskOf = [ref, mi]() -> const Mask * {
        const Sequence *seq = Session::instance()->sequence();
        const EffectInstance *fx = seq ? params::find(*seq, ref) : nullptr;
        return fx && mi < (int)fx->masks.size() ? &fx->masks[mi] : nullptr;
    };
    struct P { const char *name; AnimatedParam Mask::*member; int comps; double step; };
    const P list[] = {{"Center", &Mask::center, 2, 0.002}, {"Size", &Mask::size, 2, 0.002}, {"Feather", &Mask::feather, 1, 0.001},
                      {"Opacity", &Mask::opacity, 1, 0.005}, {"Expansion", &Mask::expansion, 1, 0.001}, {"Rotation", &Mask::rotation, 1, 0.5}};
    for (const auto &pp : list) {
        auto *box = new QWidget(w);
        auto *hl = new QHBoxLayout(box);
        hl->setContentsMargins(0, 0, 0, 0);
        std::vector<ValueSlider *> sl;
        for (int c = 0; c < pp.comps; ++c) {
            auto *v = new ValueSlider(box);
            v->setRange(-1e5, 1e5);
            v->setStep(pp.step);
            v->setDecimals(3);
            hl->addWidget(v);
            sl.push_back(v);
            auto member = pp.member;
            const QString key = ref.clipId + ref.effectUid + "mask" + QString::number(mi) + pp.name;
            connect(v, &ValueSlider::valueChanged, box, [=](double val) {
                editMask(tr("Edit Mask"), key, [=](Mask &m, Ticks t) {
                    Value cur = (m.*member).valueAt(t);
                    cur.v[c] = val;
                    (m.*member).setValue(cur, t);
                });
            });
            connect(v, &ValueSlider::editingFinished, box, [] { params::finish(); });
        }
        auto member = pp.member;
        m_refreshers.push_back([=] {
            const Mask *m = maskOf();
            const Sequence *seq = Session::instance()->sequence();
            if (!m || !seq) return;
            Value v = (m->*member).valueAt(params::localTime(*seq, ref));
            for (int c = 0; c < (int)sl.size(); ++c) sl[c]->setValue(v.v[c]);
        });
        f->addRow(trFx(pp.name), box);
    }
    auto *inv = new QCheckBox(tr("Inverted"), w);
    connect(inv, &QCheckBox::clicked, w, [=](bool on) {
        editMask(tr("Invert Mask"), {}, [on](Mask &m, Ticks) { m.inverted = on; });
        params::finish();
    });
    m_refreshers.push_back([=] { if (const Mask *m = maskOf()) { QSignalBlocker b(inv); inv->setChecked(m->inverted); } });
    auto *del = new QPushButton(tr("Delete Mask"), w);
    connect(del, &QPushButton::clicked, w, [ref, mi, s] {
        s->doc().editSequence(tr("Delete Mask"), [&](Sequence &seq) {
            EffectInstance *fx = params::find(seq, ref);
            if (!fx || mi >= (int)fx->masks.size()) return false;
            fx->masks.erase(fx->masks.begin() + mi);
            return true;
        });
    });
    auto *hl = new QHBoxLayout;
    hl->addWidget(inv);
    hl->addStretch();
    hl->addWidget(del);
    f->addRow(hl);
    return w;
}

QWidget *EffectControls::effectBlock(const Clip &clip, const EffectInstance &fx, int index, int count)
{
    const EffectDef *def = EffectRegistry::instance().find(fx.type);
    auto *content = new QWidget;
    auto *v = new QVBoxLayout(content);
    v->setContentsMargins(0, 2, 0, 4);
    v->setSpacing(0);
    const EffectRef ref{clip.id, fx.uid, false};
    if (def) {
        QString group;
        for (const auto &p : def->params) {
            if (p.group != group && !p.group.isEmpty()) {
                group = p.group;
                auto *g = new QLabel(trFx(p.group));
                g->setContentsMargins(8, 6, 0, 2);
                g->setStyleSheet("color: #9a9aa6; font-size: 10px; text-transform: uppercase;");
                v->addWidget(g);
            }
            v->addWidget(paramRow(ref, p));
        }
    }
    for (int i = 0; i < (int)fx.masks.size(); ++i) {
        auto *t = new QLabel(tr("Mask (%1)").arg(i + 1));
        t->setContentsMargins(8, 4, 0, 0);
        v->addWidget(t);
        v->addWidget(maskBlock(ref, i));
    }
    auto *sec = new Section(def ? trFx(def->name) : fx.type, content, nullptr, def && (def->intrinsic ? fx.type != "timeRemap" : true));
    auto *hdr = sec->headerWidget();
    auto *hl = static_cast<QHBoxLayout *>(hdr->layout());
    Session *s = Session::instance();
    const QString cid = clip.id, uid = fx.uid;
    auto *en = new QCheckBox(hdr);
    en->setChecked(fx.enabled);
    en->setToolTip(tr("Bypass"));
    hl->insertWidget(0, en);
    connect(en, &QCheckBox::clicked, hdr, [s, cid, uid](bool on) {
        s->doc().editSequence(tr("Toggle Effect"), [&](Sequence &seq) {
            Clip *c = seq.clip(cid);
            EffectInstance *e = c ? c->effectByUid(uid) : nullptr;
            if (!e) return false;
            e->enabled = on;
            return true;
        });
    });
    if (def && (def->supportsMask || fx.type == "opacity")) {
        for (int shape = 0; shape < 2; ++shape) {
            auto *b = new QToolButton(hdr);
            b->setAutoRaise(true);
            b->setIcon(Icons::get(shape == 0 ? "mask-ellipse" : "mask-rect"));
            b->setToolTip(shape == 0 ? tr("Create ellipse mask") : tr("Create 4-point polygon mask"));
            hl->addWidget(b);
            connect(b, &QToolButton::clicked, hdr, [s, cid, uid, shape] {
                s->doc().editSequence(tr("Add Mask"), [&](Sequence &seq) {
                    Clip *c = seq.clip(cid);
                    EffectInstance *e = c ? c->effectByUid(uid) : nullptr;
                    if (!e || e->masks.size() >= 4) return false;
                    Mask m;
                    m.shape = shape == 0 ? Mask::Ellipse : Mask::Rectangle;
                    e->masks.push_back(m);
                    return true;
                });
            });
        }
    }
    if (def && !def->intrinsic) {
        auto move = [s, cid, uid](int d) {
            s->doc().editSequence(tr("Reorder Effect"), [&](Sequence &seq) {
                Clip *c = seq.clip(cid);
                if (!c) return false;
                for (int i = 0; i < (int)c->effects.size(); ++i)
                    if (c->effects[i].uid == uid) {
                        int j = i + d;
                        if (j < 0 || j >= (int)c->effects.size()) return false;
                        const EffectDef *od = EffectRegistry::instance().find(c->effects[j].type);
                        if (od && od->intrinsic) return false;
                        std::swap(c->effects[i], c->effects[j]);
                        return true;
                    }
                return false;
            });
        };
        auto *up = new QToolButton(hdr), *down = new QToolButton(hdr), *del = new QToolButton(hdr);
        up->setArrowType(Qt::UpArrow);
        down->setArrowType(Qt::DownArrow);
        del->setIcon(Icons::get("trash"));
        del->setToolTip(tr("Remove Effect"));
        for (auto *b : {up, down, del}) { b->setAutoRaise(true); hl->addWidget(b); }
        up->setEnabled(index > 0);
        down->setEnabled(index < count - 1);
        connect(up, &QToolButton::clicked, hdr, [move] { move(-1); });
        connect(down, &QToolButton::clicked, hdr, [move] { move(1); });
        connect(del, &QToolButton::clicked, hdr, [s, cid, uid] {
            s->doc().editSequence(tr("Remove Effect"), [&](Sequence &seq) {
                Clip *c = seq.clip(cid);
                if (!c) return false;
                auto it = std::remove_if(c->effects.begin(), c->effects.end(), [&](const EffectInstance &e) { return e.uid == uid; });
                if (it == c->effects.end()) return false;
                c->effects.erase(it, c->effects.end());
                return true;
            });
        });
    }
    return sec;
}

QWidget *EffectControls::transitionBlock(const Transition &t)
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(6, 6, 6, 6);
    Session *s = Session::instance();
    const QString id = t.id;
    auto *form = new QFormLayout;
    auto *dur = new ValueSlider(w);
    dur->setRange(0.04, 60, false);
    dur->setStep(1.0 / 25);
    dur->setDecimals(2);
    dur->setSuffix(" s");
    auto editT = [s, id](const QString &label, const std::function<void(Transition &, Sequence &)> &fn) {
        s->doc().editSequence(label, [&](Sequence &seq) {
            for (auto *vv : {&seq.videoTracks, &seq.audioTracks})
                for (auto &tr : *vv)
                    if (Transition *x = tr.transition(id)) { fn(*x, seq); edit::normalize(seq); return true; }
            return false;
        });
    };
    connect(dur, &ValueSlider::editingFinished, w, [=] {
        editT(tr("Transition Duration"), [=](Transition &x, Sequence &seq) { x.duration = std::max(seq.fps.frameTicks(), floorToFrame(secondsToTicks(dur->value()), seq.fps)); });
    });
    form->addRow(tr("Duration"), dur);
    auto *align = new QComboBox(w);
    align->addItems({tr("Center at Cut"), tr("Start at Cut"), tr("End at Cut")});
    connect(align, QOverload<int>::of(&QComboBox::activated), w, [=](int i) {
        editT(tr("Transition Alignment"), [i](Transition &x, Sequence &) { x.alignment = Transition::Alignment(i); });
    });
    form->addRow(tr("Alignment"), align);
    auto *type = new QComboBox(w);
    const EffectDef *cur = EffectRegistry::instance().find(t.effect.type);
    const EffectKind kind = cur ? cur->kind : EffectKind::VideoTransition;
    for (const EffectDef *d : EffectRegistry::instance().all(kind))
        type->addItem(trFx(d->name), d->id);
    type->setCurrentIndex(type->findData(t.effect.type));
    connect(type, QOverload<int>::of(&QComboBox::activated), w, [=](int i) {
        const QString nt = type->itemData(i).toString();
        editT(tr("Change Transition"), [nt](Transition &x, Sequence &) { x.effect = EffectInstance::create(nt); });
    });
    form->addRow(tr("Type"), type);
    v->addLayout(form);
    m_refreshers.push_back([=] {
        const Sequence *seq = Session::instance()->sequence();
        if (!seq) return;
        for (const auto *vv : {&seq->videoTracks, &seq->audioTracks})
            for (const auto &tr : *vv)
                for (const auto &x : tr.transitions)
                    if (x.id == id) {
                        dur->setValue(ticksToSeconds(x.duration));
                        QSignalBlocker b(align);
                        align->setCurrentIndex(int(x.alignment));
                    }
    });
    if (cur) {
        const EffectRef ref{t.id, {}, true};
        for (const auto &p : cur->params)
            v->addWidget(paramRow(ref, p));
    }
    return w;
}

QWidget *EffectControls::titleBlock(const Clip &clip)
{
    auto *content = new QWidget;
    auto *v = new QVBoxLayout(content);
    v->setContentsMargins(6, 4, 6, 6);
    Session *s = Session::instance();
    const QString cid = clip.id;
    auto layerAt = [cid](int i) -> QJsonObject {
        const Sequence *seq = Session::instance()->sequence();
        const Clip *c = seq ? seq->clip(cid) : nullptr;
        if (!c) return {};
        return c->gen["layers"].toArray().at(i).toObject();
    };
    auto setLayer = [s, cid](int i, const QString &key, const QJsonValue &val, bool merge) {
        const QString seqId = s->project().activeSequenceId;
        s->doc().editMerged(tr("Edit Graphic"), merge ? cid + key + QString::number(i) : QString(), [&](Project &p) {
            Sequence *seq = p.sequence(seqId);
            Clip *c = seq ? seq->clip(cid) : nullptr;
            if (!c) return false;
            QJsonArray layers = c->gen["layers"].toArray();
            if (i >= layers.size()) return false;
            QJsonObject l = layers[i].toObject();
            if (key.contains('.')) {
                const QString a = key.section('.', 0, 0), b = key.section('.', 1);
                QJsonObject sub = l[a].toObject();
                sub[b] = val;
                l[a] = sub;
            } else {
                l[key] = val;
            }
            layers[i] = l;
            c->gen["layers"] = layers;
            return true;
        });
        if (!merge) s->doc().endMerge();
    };
    const QJsonArray layers = clip.gen["layers"].toArray();
    for (int i = 0; i < layers.size(); ++i) {
        const QJsonObject l = layers[i].toObject();
        const QString type = l["type"].toString();
        auto *box = new QWidget(content);
        auto *f = new QFormLayout(box);
        f->setContentsMargins(0, 2, 0, 6);
        auto *title = new QLabel(QString("<b>%1 %2</b>").arg(type == "text" ? tr("Text") : type == "rect" ? tr("Rectangle") : type == "ellipse" ? tr("Ellipse") : tr("Image")).arg(i + 1), box);
        f->addRow(title);
        if (type == "text") {
            auto *te = new QPlainTextEdit(box);
            te->setMaximumHeight(60);
            te->setPlainText(l["text"].toString());
            connect(te, &QPlainTextEdit::textChanged, box, [=] { setLayer(i, "text", te->toPlainText(), true); });
            f->addRow(tr("Text"), te);
            auto *font = new QFontComboBox(box);
            font->setCurrentFont(QFont(l["font"].toString("Inter")));
            connect(font, &QFontComboBox::currentFontChanged, box, [=](const QFont &fo) { setLayer(i, "font", fo.family(), false); });
            f->addRow(tr("Font"), font);
            auto *bold = new QCheckBox(tr("Bold"), box);
            bold->setChecked(l["bold"].toBool());
            auto *italic = new QCheckBox(tr("Italic"), box);
            italic->setChecked(l["italic"].toBool());
            connect(bold, &QCheckBox::clicked, box, [=](bool on) { setLayer(i, "bold", on, false); });
            connect(italic, &QCheckBox::clicked, box, [=](bool on) { setLayer(i, "italic", on, false); });
            auto *al = new QComboBox(box);
            al->addItems({"left", "center", "right"});
            al->setCurrentText(l["align"].toString("center"));
            connect(al, &QComboBox::textActivated, box, [=](const QString &a) { setLayer(i, "align", a, false); });
            auto *row = new QHBoxLayout;
            row->addWidget(bold);
            row->addWidget(italic);
            row->addWidget(al);
            f->addRow(tr("Style"), row);
            auto *size = new ValueSlider(box);
            size->setRange(0.005, 1, false);
            size->setStep(0.001);
            size->setDecimals(3);
            size->setValue(l["size"].toDouble(0.08));
            connect(size, &ValueSlider::valueChanged, box, [=](double val) { setLayer(i, "size", val, true); });
            connect(size, &ValueSlider::editingFinished, box, [] { params::finish(); });
            f->addRow(tr("Size"), size);
            auto *stroke = new QCheckBox(box);
            stroke->setChecked(l["stroke"].toObject()["enabled"].toBool());
            connect(stroke, &QCheckBox::clicked, box, [=](bool on) { setLayer(i, "stroke.enabled", on, false); });
            f->addRow(tr("Stroke"), stroke);
            auto *shadow = new QCheckBox(box);
            shadow->setChecked(l["shadow"].toObject()["enabled"].toBool());
            connect(shadow, &QCheckBox::clicked, box, [=](bool on) { setLayer(i, "shadow.enabled", on, false); });
            f->addRow(tr("Shadow"), shadow);
            auto *bg = new QCheckBox(box);
            bg->setChecked(l["background"].toObject()["enabled"].toBool());
            connect(bg, &QCheckBox::clicked, box, [=](bool on) { setLayer(i, "background.enabled", on, false); });
            f->addRow(tr("Background"), bg);
        }
        auto *color = new ColorButton(box);
        color->setColor(QColor(l["color"].toString("#ffffff")));
        connect(color, &ColorButton::colorChanged, box, [=](const QColor &c) { setLayer(i, "color", c.name(QColor::HexArgb), false); });
        f->addRow(tr("Color"), color);
        auto *pos = new QHBoxLayout;
        for (const char *k : {"x", "y"}) {
            auto *sl = new ValueSlider(box);
            sl->setRange(-1, 2);
            sl->setStep(0.001);
            sl->setDecimals(3);
            sl->setValue(l[k].toDouble(0.5));
            const QString key = k;
            connect(sl, &ValueSlider::valueChanged, box, [=](double val) { setLayer(i, key, val, true); });
            connect(sl, &ValueSlider::editingFinished, box, [] { params::finish(); });
            pos->addWidget(sl);
        }
        f->addRow(tr("Position"), pos);
        auto *animIn = new QComboBox(box), *animOut = new QComboBox(box);
        const QStringList anims = {"none", "fade", "slideUp", "slideDown", "slideLeft", "slideRight", "pop", "zoom", "typewriter", "wipe"};
        animIn->addItems(anims);
        animOut->addItems(anims);
        animIn->setCurrentText(l["anim"].toObject()["in"].toString("none"));
        animOut->setCurrentText(l["anim"].toObject()["out"].toString("none"));
        connect(animIn, &QComboBox::textActivated, box, [=](const QString &a) { setLayer(i, "anim.in", a, false); });
        connect(animOut, &QComboBox::textActivated, box, [=](const QString &a) { setLayer(i, "anim.out", a, false); });
        auto *ar = new QHBoxLayout;
        ar->addWidget(new QLabel(tr("In"), box));
        ar->addWidget(animIn);
        ar->addWidget(new QLabel(tr("Out"), box));
        ar->addWidget(animOut);
        f->addRow(tr("Animation"), ar);
        v->addWidget(box);
    }
    auto *add = new QPushButton(tr("Add Text Layer"), content);
    connect(add, &QPushButton::clicked, content, [s, cid] {
        s->doc().editSequence(tr("Add Text Layer"), [&](Sequence &seq) {
            Clip *c = seq.clip(cid);
            if (!c) return false;
            QJsonArray layers = c->gen["layers"].toArray();
            layers.append(TitleRenderer::textDoc(tr("New Text"))["layers"].toArray().first());
            c->gen["layers"] = layers;
            return true;
        });
    });
    v->addWidget(add);
    (void)layerAt;
    return new Section(tr("Graphics"), content);
}

} // namespace rc
