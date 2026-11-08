#pragma once
// Animatable parameters and keyframes.
#include "Time.h"
#include <array>
#include <vector>
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QJsonValue>
#include <QColor>
#include <QPointF>

namespace rc {

enum class ParamType {
    Float,   // v[0]
    Int,     // v[0]
    Bool,    // v[0] != 0
    Enum,    // v[0] = index in options
    Color,   // v[0..3] = r,g,b,a in 0..1 (linear UI values)
    Point,   // v[0], v[1] (normalised 0..1 frame coordinates unless noted)
    Angle,   // v[0] degrees
    String,  // s (single line)
    Text,    // s (multi line / rich text)
    Curve,   // s (JSON array of control points, see ColorCurves)
    File,    // s (path)
};

struct Value {
    std::array<double, 4> v{0, 0, 0, 0};
    QString s;

    Value() = default;
    Value(double x) { v[0] = x; }
    Value(double x, double y) { v[0] = x; v[1] = y; }
    Value(double r, double g, double b, double a) { v = {r, g, b, a}; }
    explicit Value(const QString &str) : s(str) {}

    double f() const { return v[0]; }
    bool b() const { return v[0] != 0.0; }
    int i() const { return (int)std::lround(v[0]); }
    QPointF pt() const { return {v[0], v[1]}; }
    QColor color() const { return QColor::fromRgbF(float(v[0]), float(v[1]), float(v[2]), float(v[3])); }
    static Value fromColor(const QColor &c) { return Value(c.redF(), c.greenF(), c.blueF(), c.alphaF()); }

    bool operator==(const Value &o) const { return v == o.v && s == o.s; }
    bool operator!=(const Value &o) const { return !(*this == o); }
};

enum class Interp { Linear, Hold, EaseIn, EaseOut, EaseInOut, Bezier };
QString interpName(Interp i);
Interp interpFromName(const QString &s);

struct Keyframe {
    Ticks time = 0;        // relative to the clip start (sequence time - clip.start)
    Value value;
    Interp interp = Interp::Linear;   // interpolation from this key to the next one
};

// A parameter value that may vary over time.
class AnimatedParam {
public:
    AnimatedParam() = default;
    explicit AnimatedParam(const Value &v) : m_static(v) {}

    bool animated() const { return m_animated && !m_keys.empty(); }
    void setAnimated(bool on, Ticks at = 0);   // turning on creates a key at `at`
    const std::vector<Keyframe> &keys() const { return m_keys; }
    std::vector<Keyframe> &keys() { return m_keys; }

    Value valueAt(Ticks t) const;          // t relative to clip start
    const Value &staticValue() const { return m_static; }
    // Sets value: when animated, adds/updates a keyframe at t, else sets the static value.
    void setValue(const Value &v, Ticks t = 0);
    void setStaticValue(const Value &v) { m_static = v; }
    int keyIndexAt(Ticks t, Ticks tolerance = 0) const;
    void addKey(const Keyframe &k);
    void removeKeyAt(Ticks t, Ticks tolerance = 0);
    void sortKeys();
    // Shifts all keyframes (used when trimming the head of a clip).
    void shiftKeys(Ticks delta);

    QJsonValue toJson() const;
    static AnimatedParam fromJson(const QJsonValue &v);

private:
    Value m_static;
    bool m_animated = false;
    std::vector<Keyframe> m_keys;
};

struct ParamDef {
    QString id;
    QString name;
    ParamType type = ParamType::Float;
    Value def;
    double min = 0, max = 1;          // UI range (soft for floats)
    double step = 0.01;
    QStringList options;              // Enum labels
    QString unit;                     // "%", "px", "dB", "°"...
    bool animatable = true;
    QString group;                    // UI grouping ("Basic", "Curves", ...)
};

QJsonObject valueToJson(const Value &v);
Value valueFromJson(const QJsonValue &j);

} // namespace rc
