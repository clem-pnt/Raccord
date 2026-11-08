#include "Param.h"
#include <QJsonArray>
#include <algorithm>

namespace rc {

QString interpName(Interp i)
{
    switch (i) {
    case Interp::Hold: return "hold";
    case Interp::EaseIn: return "easeIn";
    case Interp::EaseOut: return "easeOut";
    case Interp::EaseInOut: return "easeInOut";
    case Interp::Bezier: return "bezier";
    case Interp::Linear:
    default: return "linear";
    }
}

Interp interpFromName(const QString &s)
{
    if (s == "hold") return Interp::Hold;
    if (s == "easeIn") return Interp::EaseIn;
    if (s == "easeOut") return Interp::EaseOut;
    if (s == "easeInOut") return Interp::EaseInOut;
    if (s == "bezier") return Interp::Bezier;
    return Interp::Linear;
}

void AnimatedParam::setAnimated(bool on, Ticks at)
{
    if (on == m_animated)
        return;
    m_animated = on;
    if (on) {
        if (m_keys.empty())
            m_keys.push_back({at, m_static, Interp::Linear});
    } else {
        if (!m_keys.empty())
            m_static = valueAt(at);
        m_keys.clear();
    }
}

static double shape(double u, Interp a, Interp b)
{
    // a: interpolation leaving the left key, b: of the right key (for ease in).
    bool easeOut = a == Interp::EaseOut || a == Interp::EaseInOut || a == Interp::Bezier;
    bool easeIn = a == Interp::EaseIn || a == Interp::EaseInOut || b == Interp::EaseIn || a == Interp::Bezier;
    if (easeOut && easeIn)
        return u * u * (3 - 2 * u);
    if (easeOut)
        return 1 - (1 - u) * (1 - u);
    if (easeIn)
        return u * u;
    return u;
}

Value AnimatedParam::valueAt(Ticks t) const
{
    if (!animated())
        return m_static;
    const auto &k = m_keys;
    if (t <= k.front().time)
        return k.front().value;
    if (t >= k.back().time)
        return k.back().value;
    auto it = std::upper_bound(k.begin(), k.end(), t, [](Ticks x, const Keyframe &kf) { return x < kf.time; });
    const Keyframe &b = *it;
    const Keyframe &a = *(it - 1);
    if (a.interp == Interp::Hold)
        return a.value;
    double span = double(b.time - a.time);
    double u = span > 0 ? double(t - a.time) / span : 0.0;
    Value out = a.value;
    if (a.interp == Interp::Bezier && it - k.begin() >= 1) {
        // Catmull-Rom through neighbouring keys for smooth spatial/temporal motion.
        const Keyframe &p0 = (it - 1 == k.begin()) ? a : *(it - 2);
        const Keyframe &p3 = (it + 1 == k.end()) ? b : *(it + 1);
        double u2 = u * u, u3 = u2 * u;
        for (int c = 0; c < 4; ++c) {
            double P0 = p0.value.v[c], P1 = a.value.v[c], P2 = b.value.v[c], P3 = p3.value.v[c];
            out.v[c] = 0.5 * ((2 * P1) + (-P0 + P2) * u + (2 * P0 - 5 * P1 + 4 * P2 - P3) * u2 + (-P0 + 3 * P1 - 3 * P2 + P3) * u3);
        }
        return out;
    }
    double s = shape(u, a.interp, b.interp);
    for (int c = 0; c < 4; ++c)
        out.v[c] = a.value.v[c] + (b.value.v[c] - a.value.v[c]) * s;
    return out;
}

void AnimatedParam::setValue(const Value &v, Ticks t)
{
    if (!animated()) {
        m_static = v;
        return;
    }
    int idx = keyIndexAt(t);
    if (idx >= 0) {
        m_keys[idx].value = v;
    } else {
        Interp interp = Interp::Linear;
        // Inherit interpolation from the previous key.
        for (const auto &kf : m_keys)
            if (kf.time < t)
                interp = kf.interp;
        addKey({t, v, interp});
    }
}

int AnimatedParam::keyIndexAt(Ticks t, Ticks tolerance) const
{
    for (size_t i = 0; i < m_keys.size(); ++i)
        if (std::llabs(m_keys[i].time - t) <= tolerance)
            return (int)i;
    return -1;
}

void AnimatedParam::addKey(const Keyframe &kf)
{
    int idx = keyIndexAt(kf.time);
    if (idx >= 0)
        m_keys[idx] = kf;
    else
        m_keys.push_back(kf);
    m_animated = true;
    sortKeys();
}

void AnimatedParam::removeKeyAt(Ticks t, Ticks tolerance)
{
    int idx = keyIndexAt(t, tolerance);
    if (idx >= 0)
        m_keys.erase(m_keys.begin() + idx);
    if (m_keys.empty())
        m_animated = false;
}

void AnimatedParam::sortKeys()
{
    std::sort(m_keys.begin(), m_keys.end(), [](const Keyframe &a, const Keyframe &b) { return a.time < b.time; });
}

void AnimatedParam::shiftKeys(Ticks delta)
{
    for (auto &k : m_keys)
        k.time += delta;
}

QJsonObject valueToJson(const Value &v)
{
    QJsonObject o;
    QJsonArray a;
    int n = 4;
    while (n > 1 && v.v[n - 1] == 0.0)
        --n;
    for (int i = 0; i < n; ++i)
        a.append(v.v[i]);
    o["v"] = a;
    if (!v.s.isEmpty())
        o["s"] = v.s;
    return o;
}

Value valueFromJson(const QJsonValue &j)
{
    Value v;
    if (j.isDouble()) {
        v.v[0] = j.toDouble();
        return v;
    }
    QJsonObject o = j.toObject();
    QJsonArray a = o["v"].toArray();
    for (int i = 0; i < 4 && i < a.size(); ++i)
        v.v[i] = a[i].toDouble();
    v.s = o["s"].toString();
    return v;
}

QJsonValue AnimatedParam::toJson() const
{
    QJsonObject o;
    o["value"] = valueToJson(m_static);
    if (animated()) {
        QJsonArray keys;
        for (const auto &k : m_keys) {
            QJsonObject ko;
            ko["t"] = QString::number(k.time);
            ko["value"] = valueToJson(k.value);
            if (k.interp != Interp::Linear)
                ko["interp"] = interpName(k.interp);
            keys.append(ko);
        }
        o["keys"] = keys;
    }
    return o;
}

AnimatedParam AnimatedParam::fromJson(const QJsonValue &j)
{
    AnimatedParam p;
    QJsonObject o = j.toObject();
    p.m_static = valueFromJson(o["value"]);
    QJsonArray keys = o["keys"].toArray();
    for (const auto &kv : keys) {
        QJsonObject ko = kv.toObject();
        Keyframe k;
        k.time = ko["t"].toString().toLongLong();
        k.value = valueFromJson(ko["value"]);
        k.interp = interpFromName(ko["interp"].toString());
        p.m_keys.push_back(k);
    }
    p.m_animated = !p.m_keys.empty();
    p.sortKeys();
    return p;
}

} // namespace rc
