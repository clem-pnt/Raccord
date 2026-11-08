// Video effect and transition implementations for the Renderer.
#include "Renderer.h"
#include "Lut.h"
#include "Shaders.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>

namespace rc {

namespace {

struct Mat3 {
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    static Mat3 translate(double x, double y) { Mat3 r; r.m[0][2] = x; r.m[1][2] = y; return r; }
    static Mat3 scale(double x, double y) { Mat3 r; r.m[0][0] = x; r.m[1][1] = y; return r; }
    static Mat3 rotate(double rad)
    {
        Mat3 r;
        double c = std::cos(rad), s = std::sin(rad);
        r.m[0][0] = c; r.m[0][1] = -s; r.m[1][0] = s; r.m[1][1] = c;
        return r;
    }
    Mat3 operator*(const Mat3 &o) const
    {
        Mat3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                r.m[i][j] = 0;
                for (int k = 0; k < 3; ++k)
                    r.m[i][j] += m[i][k] * o.m[k][j];
            }
        return r;
    }
    Mat3 inverse() const
    {
        const auto &a = m;
        double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                     a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
        Mat3 r;
        if (std::abs(det) < 1e-12)
            return r;
        double id = 1.0 / det;
        r.m[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) * id;
        r.m[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * id;
        r.m[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * id;
        r.m[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) * id;
        r.m[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * id;
        r.m[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * id;
        r.m[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) * id;
        r.m[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * id;
        r.m[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * id;
        return r;
    }
    void toGl(float out[9]) const
    {
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                out[c * 3 + r] = float(m[r][c]);
    }
};

// Homography mapping the unit square to quad (p0 tl, p1 tr, p2 br, p3 bl).
Mat3 squareToQuad(QPointF p0, QPointF p1, QPointF p2, QPointF p3)
{
    double x0 = p0.x(), y0 = p0.y(), x1 = p1.x(), y1 = p1.y(), x2 = p2.x(), y2 = p2.y(), x3 = p3.x(), y3 = p3.y();
    double dx1 = x1 - x2, dx2 = x3 - x2, dx3 = x0 - x1 + x2 - x3;
    double dy1 = y1 - y2, dy2 = y3 - y2, dy3 = y0 - y1 + y2 - y3;
    Mat3 h;
    if (std::abs(dx3) < 1e-12 && std::abs(dy3) < 1e-12) {
        h.m[0][0] = x1 - x0; h.m[0][1] = x3 - x0; h.m[0][2] = x0;
        h.m[1][0] = y1 - y0; h.m[1][1] = y3 - y0; h.m[1][2] = y0;
        h.m[2][0] = 0; h.m[2][1] = 0; h.m[2][2] = 1;
        return h;
    }
    double den = dx1 * dy2 - dx2 * dy1;
    double g = (dx3 * dy2 - dx2 * dy3) / den;
    double hh = (dx1 * dy3 - dx3 * dy1) / den;
    h.m[0][0] = x1 - x0 + g * x1; h.m[0][1] = x3 - x0 + hh * x3; h.m[0][2] = x0;
    h.m[1][0] = y1 - y0 + g * y1; h.m[1][1] = y3 - y0 + hh * y3; h.m[1][2] = y0;
    h.m[2][0] = g; h.m[2][1] = hh; h.m[2][2] = 1;
    return h;
}

QColor colorOf(const Value &v) { return v.color(); }

} // namespace

// ---------------------------------------------------------------------------

GLuint Renderer::lut3d(const QString &path, int *size)
{
    if (path.isEmpty())
        return 0;
    auto it = m_luts.find(path);
    if (it != m_luts.end()) {
        *size = it->size;
        return it->tex;
    }
    LutEntry e;
    std::vector<float> rgb;
    int n = 0;
    if (loadCubeLut(path, n, rgb)) {
        auto *f = m_gl->f;
        f->glGenTextures(1, &e.tex);
        f->glBindTexture(GL_TEXTURE_3D, e.tex);
        f->glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB16F, n, n, n, 0, GL_RGB, GL_FLOAT, rgb.data());
        f->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        f->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        f->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        e.size = n;
    }
    m_luts.insert(path, e);
    *size = e.size;
    return e.tex;
}

static std::vector<std::pair<double, double>> pointsOf(const QJsonValue &v)
{
    std::vector<std::pair<double, double>> pts;
    for (const auto &p : v.toArray()) {
        QJsonArray a = p.toArray();
        if (a.size() >= 2)
            pts.push_back({a[0].toDouble(), a[1].toDouble()});
    }
    return pts;
}

GLuint Renderer::curveTexture(const QString &json, bool hue)
{
    const QString key = (hue ? "h:" : "c:") + json;
    auto it = m_curves.find(key);
    if (it != m_curves.end())
        return it.value();
    QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
    const int n = hue ? 360 : 1024;
    std::vector<float> data(size_t(n) * 4);
    if (hue) {
        const char *names[4] = {"hueSat", "hueHue", "hueLuma", "lumaSat"};
        for (int c = 0; c < 4; ++c) {
            auto v = evalCurve(pointsOf(o[names[c]]), n, c < 3, 0.5);
            for (int i = 0; i < n; ++i) data[size_t(i) * 4 + c] = v[i];
        }
    } else {
        const char *names[4] = {"master", "r", "g", "b"};
        for (int c = 0; c < 4; ++c) {
            auto v = evalCurve(pointsOf(o[names[c]]), n);
            for (int i = 0; i < n; ++i) data[size_t(i) * 4 + c] = v[i];
        }
    }
    GLuint t = 0;
    auto *f = m_gl->f;
    f->glGenTextures(1, &t);
    f->glBindTexture(GL_TEXTURE_2D, t);
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, n, 1, 0, GL_RGBA, GL_FLOAT, data.data());
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, hue ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    m_curves.insert(key, t);
    return t;
}

// ---------------------------------------------------------------------------

SurfaceRef Renderer::applyEffects(Ctx &c, const Clip &clip, Ticks local, SurfaceRef src, double clipScale, int trackIndex, Ticks seqTime)
{
    for (const auto &e : clip.effects) {
        if (!e.enabled || e.type == "motion" || e.type == "opacity" || e.type == "timeRemap" || e.type == "trackMatte")
            continue;
        const EffectDef *d = EffectRegistry::instance().find(e.type);
        if (!d || d->kind != EffectKind::Video)
            continue;
        SurfaceRef fx = applyEffect(c, e, local, src, clipScale, trackIndex, seqTime);
        if (!fx)
            continue;
        if (!e.masks.empty())
            fx = applyMask(e, local, src, fx);
        src = fx;
    }
    return src;
}

SurfaceRef Renderer::applyMask(const EffectInstance &e, Ticks local, const SurfaceRef &orig, const SurfaceRef &fx)
{
    SurfaceRef out = m_gl->surface(fx->w, fx->h);
    m_gl->use(m_gl->program("mask", shaders::mask));
    m_gl->target(out);
    m_gl->bindTexture("uOrig", 0, orig->tex);
    m_gl->bindTexture("uFx", 1, fx->tex);
    const int n = std::min<int>(4, (int)e.masks.size());
    m_gl->set("uCount", n);
    m_gl->set("uAspect", float(fx->w) / std::max(1, fx->h));
    for (int i = 0; i < n; ++i) {
        const Mask &m = e.masks[i];
        QByteArray idx = "[" + QByteArray::number(i) + "]";
        m_gl->set(("uShape" + idx).constData(), m.shape == Mask::Ellipse ? 0 : 1);
        QPointF ctr = m.center.valueAt(local).pt();
        QPointF sz = m.size.valueAt(local).pt();
        const double exp = m.expansion.valueAt(local).f();
        m_gl->set(("uCenter" + idx).constData(), float(ctr.x()), float(ctr.y()));
        m_gl->set(("uHalf" + idx).constData(), float(std::max(0.0, sz.x() / 2 + exp)), float(std::max(0.0, sz.y() / 2 + exp)));
        m_gl->set(("uFeather" + idx).constData(), float(m.feather.valueAt(local).f()));
        m_gl->set(("uOpacity" + idx).constData(), float(m.opacity.valueAt(local).f()));
        m_gl->set(("uRot" + idx).constData(), float(m.rotation.valueAt(local).f() * M_PI / 180.0));
        m_gl->set(("uInvert" + idx).constData(), m.inverted ? 1 : 0);
    }
    m_gl->drawQuad();
    return out;
}

SurfaceRef Renderer::applyEffect(Ctx &c, const EffectInstance &e, Ticks local, SurfaceRef src, double clipScale, int trackIndex, Ticks seqTime)
{
    GL &g = *m_gl;
    auto V = [&](const char *p) { return e.value(p, local); };
    auto F = [&](const char *p) { return float(e.f(p, local)); };
    auto begin = [&](const char *name, const char *source) {
        SurfaceRef out = g.surface(src->w, src->h);
        g.use(g.program(name, source));
        g.target(out);
        g.bindTexture("uTex", 0, src->tex);
        return out;
    };
    const float px = float(clipScale);       // clip pixels -> surface pixels
    const float aspect = float(src->w) / std::max(1, src->h);
    const QString &t = e.type;
    (void)trackIndex;

    if (t == "lumetri") {
        int inN = 0, lookN = 0;
        GLuint inLut = lut3d(V("inputLut").s, &inN);
        GLuint look = lut3d(V("look").s, &lookN);
        SurfaceRef out = begin("lumetri", shaders::lumetri);
        g.bindTexture("uInLut", 1, inLut, GL_TEXTURE_3D);
        g.set("uInLutSize", float(inN));
        g.set("uHasInLut", inLut ? 1 : 0);
        g.bindTexture("uLook", 2, look, GL_TEXTURE_3D);
        g.set("uLookSize", float(lookN));
        g.set("uHasLook", look ? 1 : 0);
        g.set("uLookIntensity", F("lookIntensity") / 100.f);
        const QString rgbCurve = V("curveRgb").s;
        QJsonObject hue;
        for (const char *k : {"curveHueSat", "curveHueHue", "curveHueLuma", "curveLumaSat"}) {
            QString s = V(k).s;
            if (!s.isEmpty()) {
                QJsonDocument d = QJsonDocument::fromJson(s.toUtf8());
                QString name = QString(k).mid(5);
                name[0] = name[0].toLower();
                hue[name] = d.isArray() ? QJsonValue(d.array()) : d.object()["points"];
            }
        }
        g.bindTexture("uCurve", 3, rgbCurve.isEmpty() ? 0 : curveTexture(rgbCurve, false));
        g.set("uHasCurve", rgbCurve.isEmpty() ? 0 : 1);
        g.bindTexture("uHueCurves", 4, hue.isEmpty() ? 0 : curveTexture(QString::fromUtf8(QJsonDocument(hue).toJson(QJsonDocument::Compact)), true));
        g.set("uHasHueCurves", hue.isEmpty() ? 0 : 1);
        g.set("uTemp", F("temperature") / 100.f);
        g.set("uTint", F("tint") / 100.f);
        g.set("uExposure", F("exposure"));
        g.set("uContrast", F("contrast") / 100.f);
        g.set("uHighlights", F("highlights") / 100.f);
        g.set("uShadows", F("shadows") / 100.f);
        g.set("uWhites", F("whites") / 100.f);
        g.set("uBlacks", F("blacks") / 100.f);
        g.set("uSaturation", F("saturation") / 100.f);
        g.set("uVibrance", F("vibrance") / 100.f);
        g.set("uFaded", F("fadedFilm") / 100.f);
        g.set("uSharpen", F("sharpen") / 100.f);
        g.set("uTintBalance", F("tintBalance") / 100.f);
        QColor st = colorOf(V("shadowTint")), ht = colorOf(V("highlightTint"));
        g.set("uShadowTint", float(st.redF() - 0.5), float(st.greenF() - 0.5), float(st.blueF() - 0.5));
        g.set("uHighlightTint", float(ht.redF() - 0.5), float(ht.greenF() - 0.5), float(ht.blueF() - 0.5));
        // Wheels: point (x, y) in the unit disc -> colour offset.
        auto wheel = [&](const char *p, const char *uni) {
            QPointF w = V(p).pt();
            double ang = std::atan2(-w.y(), w.x());
            double r = std::min(1.0, std::hypot(w.x(), w.y()));
            QColor hc = QColor::fromHsvF(float(std::fmod(ang / (2 * M_PI) + 1.0, 1.0)), 1.0f, 1.0f);
            double k = r * 0.25;
            double l = (hc.redF() + hc.greenF() + hc.blueF()) / 3;
            g.set(uni, float((hc.redF() - l) * k), float((hc.greenF() - l) * k), float((hc.blueF() - l) * k));
        };
        wheel("wheelShadows", "uWheelS");
        wheel("wheelMidtones", "uWheelM");
        wheel("wheelHighlights", "uWheelH");
        g.set("uLumaS", F("wheelShadowsLuma") / 100.f);
        g.set("uLumaM", F("wheelMidtonesLuma") / 100.f);
        g.set("uLumaH", F("wheelHighlightsLuma") / 100.f);
        g.set("uHsl", V("hslEnable").b() ? 1 : 0);
        g.set("uHslShowMask", (V("hslShowMask").b() && c.opt.showHslMask && !c.opt.exporting) ? 1 : 0);
        g.set("uHslHue", F("hslHue"));
        g.set("uHslRange", F("hslHueRange"));
        g.set("uHslSatMin", F("hslSatMin") / 100.f);
        g.set("uHslLumaMin", F("hslLumaMin") / 100.f);
        g.set("uHslLumaMax", F("hslLumaMax") / 100.f);
        g.set("uHslSoft", F("hslSoftness") / 100.f);
        g.set("uHslHueShift", F("hslHueShift"));
        g.set("uHslSat", F("hslSat") / 100.f);
        g.set("uHslExposure", F("hslExposure"));
        g.set("uVigAmount", F("vignetteAmount"));
        g.set("uVigMid", F("vignetteMidpoint") / 100.f);
        g.set("uVigRound", F("vignetteRoundness") / 100.f);
        g.set("uVigFeather", F("vignetteFeather") / 100.f);
        g.set("uTexel", 1.f / src->w, 1.f / src->h);
        g.set("uAspect", aspect);
        g.drawQuad();
        return out;
    }
    if (t == "lut") {
        int n = 0;
        GLuint lt = lut3d(V("file").s, &n);
        if (!lt)
            return src;
        SurfaceRef out = begin("lut", shaders::lut);
        g.bindTexture("uLut", 1, lt, GL_TEXTURE_3D);
        g.set("uSize", float(n));
        g.set("uIntensity", F("intensity") / 100.f);
        g.drawQuad();
        return out;
    }
    static const QStringList simple = {"brightnessContrast", "hueSaturation", "levels", "colorBalance", "channelMixer", "tint",
                                       "blackWhite", "invert", "posterize", "threshold"};
    if (int op = simple.indexOf(t); op >= 0) {
        SurfaceRef out = begin("simpleColor", shaders::simpleColor);
        float p[12] = {0};
        QColor cc[3];
        switch (op) {
        case 0: p[0] = F("brightness") / 200.f; p[1] = F("contrast") / 100.f; break;
        case 1: p[0] = F("hue"); p[1] = F("saturation") / 100.f; p[2] = F("lightness") / 100.f; p[3] = V("colorize").b() ? 1.f : 0.f; break;
        case 2: p[0] = F("inBlack"); p[1] = F("inWhite"); p[2] = F("gamma"); p[3] = F("outBlack"); p[4] = F("outWhite"); break;
        case 3:
            cc[0] = colorOf(V("lift")); cc[1] = colorOf(V("gamma")); cc[2] = colorOf(V("gain"));
            p[0] = F("liftMaster") * 0.5f; p[1] = F("gammaMaster"); p[2] = F("gainMaster");
            break;
        case 4: {
            const char *k[9] = {"rr", "rg", "rb", "gr", "gg", "gb", "br", "bg", "bb"};
            for (int i = 0; i < 9; ++i) p[i] = F(k[i]) / 100.f;
            p[9] = V("monochrome").b() ? 1.f : 0.f;
            break;
        }
        case 5: cc[0] = colorOf(V("black")); cc[1] = colorOf(V("white")); p[0] = F("amount") / 100.f; break;
        case 7: p[0] = F("blend") / 100.f; break;
        case 8: p[0] = F("levels"); break;
        case 9: p[0] = F("level"); break;
        default: break;
        }
        g.set("uOp", op);
        for (int i = 0; i < 12; ++i)
            g.set(("uP[" + QByteArray::number(i) + "]").constData(), p[i]);
        for (int i = 0; i < 3; ++i) {
            QColor col = cc[i].isValid() ? cc[i] : QColor(128, 128, 128);
            if (op == 3)
                g.set(("uC[" + QByteArray::number(i) + "]").constData(), float(col.redF() - 0.5), float(col.greenF() - 0.5), float(col.blueF() - 0.5), 1.f);
            else
                g.set(("uC[" + QByteArray::number(i) + "]").constData(), float(col.redF()), float(col.greenF()), float(col.blueF()), 1.f);
        }
        g.drawQuad();
        return out;
    }
    if (t == "gaussianBlur" || t == "blurRegion") {
        if (t == "blurRegion" && V("mosaic").i() > 0) {
            SurfaceRef out = begin("stylize", shaders::stylize);
            g.set("uOp", 0);
            g.set("uP[0]", float(V("mosaic").i()));
            g.set("uAspect", aspect);
            g.drawQuad();
            return out;
        }
        int dims = t == "gaussianBlur" ? V("dimensions").i() : 0;
        bool rep = t == "gaussianBlur" ? V("repeatEdges").b() : true;
        return blur(src, F("radius") * px, dims, rep);
    }
    if (t == "directionalBlur" || t == "zoomBlur") {
        SurfaceRef out = begin("dirBlur", shaders::dirBlur);
        if (t == "directionalBlur") {
            double a = F("direction") * M_PI / 180.0;
            double len = F("length") * px;
            g.set("uOp", 0);
            g.set("uVec", float(std::sin(a) * len / src->w), float(-std::cos(a) * len / src->h));
        } else {
            QPointF ctr = V("center").pt();
            g.set("uOp", 1);
            g.set("uCenter", float(ctr.x()), float(ctr.y()));
            g.set("uAmount", F("amount") / 100.f * 0.5f);
        }
        g.drawQuad();
        return out;
    }
    if (t == "sharpen") {
        SurfaceRef bl = blur(src, F("radius") * px, 0, true);
        SurfaceRef out = begin("sharpen", shaders::sharpen);
        g.bindTexture("uBlur", 1, bl->tex);
        g.set("uAmount", F("amount") / 100.f);
        g.set("uThreshold", F("threshold"));
        g.drawQuad();
        return out;
    }
    if (t == "glow") {
        SurfaceRef bright = begin("threshold", shaders::threshold);
        g.set("uThreshold", F("threshold") / 100.f);
        g.drawQuad();
        SurfaceRef bl = blur(bright, F("radius") * px, 0, false);
        SurfaceRef out = begin("glowCombine", shaders::glowCombine);
        g.bindTexture("uGlow", 1, bl->tex);
        g.set("uIntensity", F("intensity"));
        QColor col = colorOf(V("color"));
        g.set("uColor", float(col.redF()), float(col.greenF()), float(col.blueF()));
        g.drawQuad();
        return out;
    }
    if (t == "crop") {
        SurfaceRef out = begin("crop", shaders::crop);
        g.set("uCrop", F("left") / 100.f, F("top") / 100.f, F("right") / 100.f, F("bottom") / 100.f);
        g.set("uFeather", F("feather") * px / src->w, F("feather") * px / src->h);
        g.set("uZoom", V("zoom").b() ? 1 : 0);
        g.drawQuad();
        return out;
    }
    if (t == "flip" || t == "mirror" || t == "lensDistortion" || t == "wave" || t == "cornerPin" || t == "transform" || t == "stabilizer") {
        SurfaceRef out = begin("distort", shaders::distort);
        float p[8] = {0};
        int op = 0;
        float mat[9];
        Mat3 M;
        float opacity = 1.f;
        if (t == "flip") { op = 0; p[0] = V("horizontal").b(); p[1] = V("vertical").b(); }
        else if (t == "mirror") { op = 1; QPointF ctr = V("center").pt(); p[0] = ctr.x(); p[1] = ctr.y(); p[2] = F("angle") * M_PI / 180.0; }
        else if (t == "lensDistortion") { op = 2; p[0] = F("curvature") / 100.f; p[1] = F("zoom") / 100.f; }
        else if (t == "wave") {
            op = 3;
            p[0] = F("height") * px / src->h;
            p[1] = F("width") * px / src->h;
            p[2] = F("direction") * M_PI / 180.0;
            p[3] = float(ticksToSeconds(local) * F("speed") * 2 * M_PI);
        } else if (t == "cornerPin") {
            op = 4;
            M = squareToQuad(V("tl").pt(), V("tr").pt(), V("br").pt(), V("bl").pt()).inverse();
        } else if (t == "transform") {
            op = 4;
            const double W = src->w, H = src->h;
            QPointF P = V("position").pt(), A = V("anchor").pt();
            double s = std::max(0.0001, double(F("scale")) / 100.0);
            double rot = F("rotation") * M_PI / 180.0;
            double skew = std::tan(std::clamp(double(F("skew")), -85.0, 85.0) * M_PI / 180.0);
            double axis = F("skewAxis") * M_PI / 180.0;
            Mat3 K;
            K.m[0][1] = skew;
            Mat3 Ka = Mat3::rotate(axis) * K * Mat3::rotate(-axis);
            Mat3 fwd = Mat3::translate(P.x() * W, P.y() * H) * Mat3::rotate(rot) * Ka * Mat3::scale(s, s) * Mat3::translate(-A.x() * W, -A.y() * H);
            M = Mat3::scale(1.0 / W, 1.0 / H) * fwd.inverse() * Mat3::scale(W, H);
            opacity = F("opacity") / 100.f;
        } else {
            // Stabiliser: analysis = JSON array of per-frame [dx, dy, rot, scale] corrections (uv units).
            op = 4;
            QJsonArray a = QJsonDocument::fromJson(V("analysis").s.toUtf8()).array();
            if (a.isEmpty())
                return src;
            int fi = std::clamp<int>(int(ticksToFrame(local, c.seq->fps)), 0, a.size() - 1);
            QJsonArray v = a[fi].toArray();
            double crop = F("crop") / 100.0;
            double dx = v[0].toDouble(), dy = v[1].toDouble(), rot = v.size() > 2 ? v[2].toDouble() : 0, sc = v.size() > 3 ? v[3].toDouble() : 1;
            const double W = src->w, H = src->h;
            Mat3 fwd = Mat3::translate(0.5 * W + dx * W, 0.5 * H + dy * H) * Mat3::rotate(rot) * Mat3::scale(crop * sc, crop * sc) * Mat3::translate(-0.5 * W, -0.5 * H);
            M = Mat3::scale(1.0 / W, 1.0 / H) * fwd.inverse() * Mat3::scale(W, H);
        }
        M.toGl(mat);
        g.set("uOp", op);
        for (int i = 0; i < 8; ++i)
            g.set(("uP[" + QByteArray::number(i) + "]").constData(), p[i]);
        g.setMat3("uMat", mat);
        g.set("uAspect", aspect);
        g.set("uOpacity", opacity);
        g.drawQuad();
        return out;
    }
    if (t == "chromaKey" || t == "lumaKey") {
        SurfaceRef out = begin("keying", shaders::keying);
        if (t == "chromaKey") {
            QColor k = colorOf(V("keyColor"));
            g.set("uOp", 0);
            g.set("uKey", float(k.redF()), float(k.greenF()), float(k.blueF()));
            g.set("uP[0]", F("tolerance") / 100.f);
            g.set("uP[1]", F("softness") / 100.f);
            g.set("uP[2]", F("choke") / 100.f);
            g.set("uP[3]", F("spill") / 100.f);
            g.set("uP[4]", F("despill") / 100.f);
            g.set("uP[5]", (V("showMatte").b() && !c.opt.exporting) ? 1.f : 0.f);
        } else {
            g.set("uOp", 1);
            g.set("uP[0]", F("threshold") / 100.f);
            g.set("uP[1]", F("cutoff") / 100.f + 0.02f);
            g.set("uP[2]", V("invert").b() ? 1.f : 0.f);
        }
        g.drawQuad();
        return out;
    }
    static const QStringList styl = {"mosaic", "filmGrain", "vignette", "emboss", "findEdges", "chromaticAberration", "letterbox", "gradient"};
    if (int op = styl.indexOf(t); op >= 0) {
        SurfaceRef out = begin("stylize", shaders::stylize);
        float p[8] = {0};
        QColor c0, c1;
        switch (op) {
        case 0: p[0] = float(V("blocks").i()); break;
        case 1: p[0] = F("amount") / 100.f; p[1] = F("size") * px; p[2] = V("monochrome").b(); break;
        case 2: p[0] = F("amount") / 100.f; p[1] = F("size") / 100.f; p[2] = F("feather") / 100.f; c0 = colorOf(V("color")); break;
        case 3: p[0] = F("direction") * M_PI / 180.0; p[1] = F("relief") * px; break;
        case 4: p[0] = V("invert").b(); p[1] = F("blend") / 100.f; break;
        case 5: p[0] = F("amount") * px; break;
        case 6: p[0] = F("ratio"); c0 = colorOf(V("color")); break;
        case 7: {
            QPointF a = V("start").pt(), b = V("end").pt();
            p[0] = a.x(); p[1] = a.y(); p[2] = b.x(); p[3] = b.y(); p[4] = V("shape").i(); p[5] = F("blend") / 100.f;
            c0 = colorOf(V("startColor")); c1 = colorOf(V("endColor"));
            break;
        }
        default: break;
        }
        g.set("uOp", op);
        for (int i = 0; i < 8; ++i)
            g.set(("uP[" + QByteArray::number(i) + "]").constData(), p[i]);
        if (!c0.isValid()) c0 = Qt::black;
        if (!c1.isValid()) c1 = Qt::white;
        g.set("uC0", float(c0.redF()), float(c0.greenF()), float(c0.blueF()), float(c0.alphaF()));
        g.set("uC1", float(c1.redF()), float(c1.greenF()), float(c1.blueF()), float(c1.alphaF()));
        g.set("uTexel", 1.f / src->w, 1.f / src->h);
        g.set("uAspect", op == 6 ? float(c.seq->width) / std::max(1, c.seq->height) : aspect);
        g.set("uTime", float(ticksToSeconds(seqTime)));
        g.drawQuad();
        return out;
    }
    return src;
}

SurfaceRef Renderer::transition(Ctx &c, const Transition &tr, double progress, const SurfaceRef &a, const SurfaceRef &b)
{
    GL &g = *m_gl;
    const EffectInstance &e = tr.effect;
    const Ticks local = 0;
    auto F = [&](const char *p) { return float(e.f(p, local)); };
    static const QStringList ops = {"crossDissolve", "filmDissolve", "additiveDissolve", "dipToBlack", "dipToWhite", "blurDissolve",
                                    "wipe", "clockWipe", "irisRound", "irisBox", "barnDoor", "push", "slide", "zoom", "spin", "whip",
                                    "glitch", "lumaFade"};
    int op = ops.indexOf(e.type);
    if (op < 0) op = 0;
    SurfaceRef A = a, B = b;
    if (op == 5) {
        // Blur both sides, strongest in the middle.
        float r = F("radius") * float(std::sin(progress * M_PI)) * float(c.renderScale);
        A = blur(a, r, 0, true);
        B = blur(b, r, 0, true);
        op = 0;
    }
    SurfaceRef out = g.surface(a->w, a->h);
    g.use(g.program("transition", shaders::transition));
    g.target(out);
    g.bindTexture("uA", 0, A->tex);
    g.bindTexture("uB", 1, B->tex);
    g.set("uProgress", float(progress));
    g.set("uOp", op);
    g.set("uAspect", float(a->w) / std::max(1, a->h));
    float p[8] = {0};
    switch (op) {
    case 6: p[0] = F("angle") * M_PI / 180.0; p[1] = F("feather") / 100.f; break;
    case 7: p[0] = F("feather") / 100.f; break;
    case 8:
    case 9: {
        QPointF ctr = e.value("center", local).pt();
        p[0] = ctr.x(); p[1] = ctr.y(); p[2] = F("feather") / 100.f;
        break;
    }
    case 10: p[0] = e.value("vertical", local).b(); break;
    case 11:
    case 12:
    case 15: p[0] = float(e.value("direction", local).i()); break;
    case 13: p[0] = F("strength"); break;
    case 16: p[0] = F("intensity") / 100.f; break;
    case 17: p[0] = F("softness") / 100.f; p[1] = e.value("invert", local).b(); break;
    default: break;
    }
    for (int i = 0; i < 8; ++i)
        g.set(("uP[" + QByteArray::number(i) + "]").constData(), p[i]);
    g.drawQuad();
    return out;
}

} // namespace rc
