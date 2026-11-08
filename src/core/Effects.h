#pragma once
// Effect metadata registry and effect instances. Processing lives in
// render/VideoEffects (GLSL) and audio/AudioEffects (DSP).
#include "Param.h"
#include <map>
#include <memory>
#include <QJsonObject>

namespace rc {

enum class EffectKind { Video, Audio, VideoTransition, AudioTransition };

struct EffectDef {
    QString id;            // stable identifier stored in projects
    QString name;          // display name
    QString category;      // folder in the effects browser
    EffectKind kind = EffectKind::Video;
    std::vector<ParamDef> params;
    bool intrinsic = false;   // Motion / Opacity / Volume... always present, cannot be removed
    bool supportsMask = false;
    QString description;

    const ParamDef *param(const QString &pid) const;
};

// A mask limiting where an effect applies (or, on Opacity, the clip visibility).
struct Mask {
    enum Shape { Ellipse, Rectangle, Polygon };
    Shape shape = Ellipse;
    // Normalised clip coordinates. For Ellipse/Rectangle: points[0] = centre, size = (w, h).
    std::vector<QPointF> points;
    AnimatedParam center{Value(0.5, 0.5)};
    AnimatedParam size{Value(0.4, 0.4)};
    AnimatedParam feather{Value(0.02)};
    AnimatedParam opacity{Value(1.0)};
    AnimatedParam expansion{Value(0.0)};
    AnimatedParam rotation{Value(0.0)};
    bool inverted = false;

    QJsonObject toJson() const;
    static Mask fromJson(const QJsonObject &o);
};

struct EffectInstance {
    QString uid;
    QString type;            // EffectDef::id
    bool enabled = true;
    std::map<QString, AnimatedParam> params;
    std::vector<Mask> masks;

    // Value of a parameter at a time relative to the clip start; falls back to the definition default.
    Value value(const QString &pid, Ticks t) const;
    double f(const QString &pid, Ticks t) const { return value(pid, t).f(); }
    AnimatedParam &param(const QString &pid);
    bool hasAnimation() const;

    QJsonObject toJson() const;
    static EffectInstance fromJson(const QJsonObject &o);
    static EffectInstance create(const QString &type);
};

class EffectRegistry {
public:
    static EffectRegistry &instance();
    const EffectDef *find(const QString &id) const;
    std::vector<const EffectDef *> all(EffectKind kind) const;
    QStringList categories(EffectKind kind) const;
    void add(EffectDef def);

private:
    EffectRegistry();
    std::map<QString, EffectDef> m_defs;
    std::vector<QString> m_order;
};

// Well known blend modes (Opacity effect).
enum class BlendMode {
    Normal, Darken, Multiply, ColorBurn, LinearBurn, Lighten, Screen, ColorDodge,
    LinearDodge, Overlay, SoftLight, HardLight, VividLight, LinearLight, PinLight,
    HardMix, Difference, Exclusion, Subtract, Divide, Hue, Saturation, Color, Luminosity,
    Count
};
QStringList blendModeNames();

} // namespace rc
