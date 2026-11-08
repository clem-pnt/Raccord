#include "Effects.h"
#include <QJsonArray>
#include <QUuid>

namespace rc {

const ParamDef *EffectDef::param(const QString &pid) const
{
    for (const auto &p : params)
        if (p.id == pid)
            return &p;
    return nullptr;
}

QStringList blendModeNames()
{
    return {"Normal", "Darken", "Multiply", "Color Burn", "Linear Burn", "Lighten", "Screen",
            "Color Dodge", "Linear Dodge (Add)", "Overlay", "Soft Light", "Hard Light", "Vivid Light",
            "Linear Light", "Pin Light", "Hard Mix", "Difference", "Exclusion", "Subtract", "Divide",
            "Hue", "Saturation", "Color", "Luminosity"};
}

// ---- Mask -------------------------------------------------------------------

QJsonObject Mask::toJson() const
{
    QJsonObject o;
    o["shape"] = shape == Ellipse ? "ellipse" : shape == Rectangle ? "rectangle" : "polygon";
    QJsonArray pts;
    for (const auto &p : points)
        pts.append(QJsonArray{p.x(), p.y()});
    if (!pts.isEmpty())
        o["points"] = pts;
    o["center"] = center.toJson();
    o["size"] = size.toJson();
    o["feather"] = feather.toJson();
    o["opacity"] = opacity.toJson();
    o["expansion"] = expansion.toJson();
    o["rotation"] = rotation.toJson();
    o["inverted"] = inverted;
    return o;
}

Mask Mask::fromJson(const QJsonObject &o)
{
    Mask m;
    QString s = o["shape"].toString();
    m.shape = s == "rectangle" ? Rectangle : s == "polygon" ? Polygon : Ellipse;
    for (const auto &pv : o["points"].toArray()) {
        QJsonArray a = pv.toArray();
        m.points.push_back({a[0].toDouble(), a[1].toDouble()});
    }
    if (o.contains("center")) m.center = AnimatedParam::fromJson(o["center"]);
    if (o.contains("size")) m.size = AnimatedParam::fromJson(o["size"]);
    if (o.contains("feather")) m.feather = AnimatedParam::fromJson(o["feather"]);
    if (o.contains("opacity")) m.opacity = AnimatedParam::fromJson(o["opacity"]);
    if (o.contains("expansion")) m.expansion = AnimatedParam::fromJson(o["expansion"]);
    if (o.contains("rotation")) m.rotation = AnimatedParam::fromJson(o["rotation"]);
    m.inverted = o["inverted"].toBool();
    return m;
}

// ---- EffectInstance ---------------------------------------------------------

Value EffectInstance::value(const QString &pid, Ticks t) const
{
    auto it = params.find(pid);
    if (it != params.end())
        return it->second.valueAt(t);
    if (const EffectDef *d = EffectRegistry::instance().find(type))
        if (const ParamDef *p = d->param(pid))
            return p->def;
    return {};
}

AnimatedParam &EffectInstance::param(const QString &pid)
{
    auto it = params.find(pid);
    if (it == params.end()) {
        Value def;
        if (const EffectDef *d = EffectRegistry::instance().find(type))
            if (const ParamDef *p = d->param(pid))
                def = p->def;
        it = params.emplace(pid, AnimatedParam(def)).first;
    }
    return it->second;
}

bool EffectInstance::hasAnimation() const
{
    for (const auto &[k, p] : params)
        if (p.animated())
            return true;
    return false;
}

QJsonObject EffectInstance::toJson() const
{
    QJsonObject o;
    o["uid"] = uid;
    o["type"] = type;
    if (!enabled)
        o["enabled"] = false;
    QJsonObject po;
    for (const auto &[k, p] : params)
        po[k] = p.toJson();
    o["params"] = po;
    if (!masks.empty()) {
        QJsonArray ma;
        for (const auto &m : masks)
            ma.append(m.toJson());
        o["masks"] = ma;
    }
    return o;
}

EffectInstance EffectInstance::fromJson(const QJsonObject &o)
{
    EffectInstance e;
    e.uid = o["uid"].toString();
    if (e.uid.isEmpty())
        e.uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    e.type = o["type"].toString();
    e.enabled = o["enabled"].toBool(true);
    QJsonObject po = o["params"].toObject();
    for (auto it = po.begin(); it != po.end(); ++it)
        e.params[it.key()] = AnimatedParam::fromJson(it.value());
    for (const auto &mv : o["masks"].toArray())
        e.masks.push_back(Mask::fromJson(mv.toObject()));
    return e;
}

EffectInstance EffectInstance::create(const QString &type)
{
    EffectInstance e;
    e.uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    e.type = type;
    if (const EffectDef *d = EffectRegistry::instance().find(type))
        for (const auto &p : d->params)
            e.params[p.id] = AnimatedParam(p.def);
    return e;
}

// ---- Registry ----------------------------------------------------------------

namespace {
ParamDef F(const QString &id, const QString &name, double def, double mn, double mx, const QString &unit = {}, double step = 0.01, const QString &group = {})
{
    ParamDef p; p.id = id; p.name = name; p.type = ParamType::Float; p.def = Value(def);
    p.min = mn; p.max = mx; p.unit = unit; p.step = step; p.group = group; return p;
}
ParamDef A(const QString &id, const QString &name, double def, const QString &group = {})
{
    ParamDef p = F(id, name, def, -360, 360, "°", 0.1, group); p.type = ParamType::Angle; return p;
}
ParamDef B(const QString &id, const QString &name, bool def, const QString &group = {})
{
    ParamDef p; p.id = id; p.name = name; p.type = ParamType::Bool; p.def = Value(def ? 1.0 : 0.0);
    p.min = 0; p.max = 1; p.animatable = false; p.group = group; return p;
}
ParamDef E(const QString &id, const QString &name, int def, const QStringList &opts, const QString &group = {})
{
    ParamDef p; p.id = id; p.name = name; p.type = ParamType::Enum; p.def = Value(def); p.options = opts;
    p.min = 0; p.max = opts.size() - 1; p.animatable = false; p.group = group; return p;
}
ParamDef C(const QString &id, const QString &name, QColor c, const QString &group = {})
{
    ParamDef p; p.id = id; p.name = name; p.type = ParamType::Color; p.def = Value::fromColor(c); p.group = group; return p;
}
ParamDef P(const QString &id, const QString &name, double x, double y, const QString &group = {})
{
    ParamDef p; p.id = id; p.name = name; p.type = ParamType::Point; p.def = Value(x, y);
    p.min = -2; p.max = 3; p.step = 0.001; p.group = group; return p;
}
ParamDef S(const QString &id, const QString &name, const QString &def, ParamType t = ParamType::String, const QString &group = {})
{
    ParamDef p; p.id = id; p.name = name; p.type = t; p.def = Value(def); p.animatable = false; p.group = group; return p;
}
ParamDef I(const QString &id, const QString &name, int def, int mn, int mx, const QString &group = {})
{
    ParamDef p = F(id, name, def, mn, mx, {}, 1, group); p.type = ParamType::Int; return p;
}

EffectDef V(const QString &id, const QString &name, const QString &cat, std::vector<ParamDef> params, bool mask = true)
{
    EffectDef d; d.id = id; d.name = name; d.category = cat; d.kind = EffectKind::Video;
    d.params = std::move(params); d.supportsMask = mask; return d;
}
EffectDef Au(const QString &id, const QString &name, const QString &cat, std::vector<ParamDef> params)
{
    EffectDef d; d.id = id; d.name = name; d.category = cat; d.kind = EffectKind::Audio;
    d.params = std::move(params); return d;
}
EffectDef VT(const QString &id, const QString &name, const QString &cat, std::vector<ParamDef> params = {})
{
    EffectDef d; d.id = id; d.name = name; d.category = cat; d.kind = EffectKind::VideoTransition;
    d.params = std::move(params); return d;
}
EffectDef AT(const QString &id, const QString &name)
{
    EffectDef d; d.id = id; d.name = name; d.category = "Crossfade"; d.kind = EffectKind::AudioTransition; return d;
}
} // namespace

EffectRegistry &EffectRegistry::instance()
{
    static EffectRegistry r;
    return r;
}

const EffectDef *EffectRegistry::find(const QString &id) const
{
    auto it = m_defs.find(id);
    return it == m_defs.end() ? nullptr : &it->second;
}

std::vector<const EffectDef *> EffectRegistry::all(EffectKind kind) const
{
    std::vector<const EffectDef *> out;
    for (const auto &id : m_order) {
        const EffectDef &d = m_defs.at(id);
        if (d.kind == kind && !d.intrinsic)
            out.push_back(&d);
    }
    return out;
}

QStringList EffectRegistry::categories(EffectKind kind) const
{
    QStringList c;
    for (const auto *d : all(kind))
        if (!c.contains(d->category))
            c << d->category;
    return c;
}

void EffectRegistry::add(EffectDef def)
{
    if (!m_defs.count(def.id))
        m_order.push_back(def.id);
    m_defs[def.id] = std::move(def);
}

EffectRegistry::EffectRegistry()
{
    const QStringList blends = blendModeNames();

    // ---------------- Intrinsic video ----------------
    {
        EffectDef d = V("motion", "Motion", "Intrinsic", {
            P("position", "Position", 0.5, 0.5),
            F("scale", "Scale", 100, 0, 1000, "%", 0.1),
            F("scaleWidth", "Scale Width", 100, 0, 1000, "%", 0.1),
            B("uniform", "Uniform Scale", true),
            A("rotation", "Rotation", 0),
            P("anchor", "Anchor Point", 0.5, 0.5),
            F("antiFlicker", "Anti-flicker Filter", 0, 0, 1),
            E("fit", "Frame Fit", 0, {"Fit (keep aspect)", "Fill (crop)", "Stretch", "Native size"}),
        }, false);
        d.intrinsic = true;
        add(d);
    }
    {
        EffectDef d = V("opacity", "Opacity", "Intrinsic", {
            F("opacity", "Opacity", 100, 0, 100, "%", 0.1),
            E("blend", "Blend Mode", 0, blends),
        }, true);
        d.intrinsic = true;
        add(d);
    }
    {
        EffectDef d = V("timeRemap", "Time Remapping", "Intrinsic", {
            F("speed", "Speed", 100, -1000, 1000, "%", 1),
            B("frameBlend", "Frame Blending", false),
        }, false);
        d.intrinsic = true;
        add(d);
    }

    // ---------------- Colour ----------------
    add(V("lumetri", "Lumetri Color", "Color Correction", {
        // Basic correction
        S("inputLut", "Input LUT", "", ParamType::File, "Basic Correction"),
        F("temperature", "Temperature", 0, -100, 100, {}, 0.1, "Basic Correction"),
        F("tint", "Tint", 0, -100, 100, {}, 0.1, "Basic Correction"),
        F("exposure", "Exposure", 0, -5, 5, "EV", 0.01, "Basic Correction"),
        F("contrast", "Contrast", 0, -100, 100, {}, 0.1, "Basic Correction"),
        F("highlights", "Highlights", 0, -100, 100, {}, 0.1, "Basic Correction"),
        F("shadows", "Shadows", 0, -100, 100, {}, 0.1, "Basic Correction"),
        F("whites", "Whites", 0, -100, 100, {}, 0.1, "Basic Correction"),
        F("blacks", "Blacks", 0, -100, 100, {}, 0.1, "Basic Correction"),
        F("saturation", "Saturation", 100, 0, 200, "%", 0.1, "Basic Correction"),
        F("vibrance", "Vibrance", 0, -100, 100, {}, 0.1, "Creative"),
        // Creative
        S("look", "Look (LUT)", "", ParamType::File, "Creative"),
        F("lookIntensity", "Look Intensity", 100, 0, 200, "%", 0.1, "Creative"),
        F("fadedFilm", "Faded Film", 0, 0, 100, {}, 0.1, "Creative"),
        F("sharpen", "Sharpen", 0, -100, 100, {}, 0.1, "Creative"),
        C("shadowTint", "Shadow Tint", QColor(128, 128, 128), "Creative"),
        C("highlightTint", "Highlight Tint", QColor(128, 128, 128), "Creative"),
        F("tintBalance", "Tint Balance", 0, -100, 100, {}, 0.1, "Creative"),
        // Curves
        S("curveRgb", "RGB Curves", "", ParamType::Curve, "Curves"),
        S("curveHueSat", "Hue vs Saturation", "", ParamType::Curve, "Curves"),
        S("curveHueHue", "Hue vs Hue", "", ParamType::Curve, "Curves"),
        S("curveHueLuma", "Hue vs Luma", "", ParamType::Curve, "Curves"),
        S("curveLumaSat", "Luma vs Saturation", "", ParamType::Curve, "Curves"),
        // Color wheels (offset rgb in -1..1 encoded around 0)
        P("wheelShadows", "Shadows", 0, 0, "Color Wheels"),
        F("wheelShadowsLuma", "Shadows Level", 0, -100, 100, {}, 0.1, "Color Wheels"),
        P("wheelMidtones", "Midtones", 0, 0, "Color Wheels"),
        F("wheelMidtonesLuma", "Midtones Level", 0, -100, 100, {}, 0.1, "Color Wheels"),
        P("wheelHighlights", "Highlights", 0, 0, "Color Wheels"),
        F("wheelHighlightsLuma", "Highlights Level", 0, -100, 100, {}, 0.1, "Color Wheels"),
        // HSL secondary
        B("hslEnable", "Enable HSL Secondary", false, "HSL Secondary"),
        F("hslHue", "Key Hue", 0, 0, 360, "°", 0.5, "HSL Secondary"),
        F("hslHueRange", "Hue Range", 30, 0, 180, "°", 0.5, "HSL Secondary"),
        F("hslSatMin", "Saturation Min", 10, 0, 100, "%", 0.1, "HSL Secondary"),
        F("hslLumaMin", "Luma Min", 0, 0, 100, "%", 0.1, "HSL Secondary"),
        F("hslLumaMax", "Luma Max", 100, 0, 100, "%", 0.1, "HSL Secondary"),
        F("hslSoftness", "Softness", 10, 0, 100, {}, 0.1, "HSL Secondary"),
        F("hslHueShift", "Hue Shift", 0, -180, 180, "°", 0.5, "HSL Secondary"),
        F("hslSat", "Saturation", 100, 0, 200, "%", 0.1, "HSL Secondary"),
        F("hslExposure", "Exposure", 0, -3, 3, "EV", 0.01, "HSL Secondary"),
        B("hslShowMask", "Show Mask", false, "HSL Secondary"),
        // Vignette
        F("vignetteAmount", "Vignette Amount", 0, -5, 5, {}, 0.01, "Vignette"),
        F("vignetteMidpoint", "Midpoint", 50, 0, 100, {}, 0.1, "Vignette"),
        F("vignetteRoundness", "Roundness", 0, -100, 100, {}, 0.1, "Vignette"),
        F("vignetteFeather", "Feather", 50, 0, 100, {}, 0.1, "Vignette"),
    }));
    add(V("lut", "Apply LUT", "Color Correction", {
        S("file", "LUT File (.cube)", "", ParamType::File),
        F("intensity", "Intensity", 100, 0, 100, "%", 0.1),
    }));
    add(V("brightnessContrast", "Brightness & Contrast", "Color Correction", {
        F("brightness", "Brightness", 0, -100, 100), F("contrast", "Contrast", 0, -100, 100),
    }));
    add(V("hueSaturation", "Hue / Saturation", "Color Correction", {
        A("hue", "Hue", 0), F("saturation", "Saturation", 0, -100, 100), F("lightness", "Lightness", 0, -100, 100),
        B("colorize", "Colorize", false),
    }));
    add(V("levels", "Levels", "Color Correction", {
        F("inBlack", "Input Black", 0, 0, 1, {}, 0.001), F("inWhite", "Input White", 1, 0, 1, {}, 0.001),
        F("gamma", "Gamma", 1, 0.1, 5, {}, 0.01),
        F("outBlack", "Output Black", 0, 0, 1, {}, 0.001), F("outWhite", "Output White", 1, 0, 1, {}, 0.001),
    }));
    add(V("colorBalance", "Color Balance (Lift Gamma Gain)", "Color Correction", {
        C("lift", "Lift", QColor(128, 128, 128)), C("gamma", "Gamma", QColor(128, 128, 128)), C("gain", "Gain", QColor(128, 128, 128)),
        F("liftMaster", "Lift Master", 0, -1, 1), F("gammaMaster", "Gamma Master", 0, -1, 1), F("gainMaster", "Gain Master", 0, -1, 1),
    }));
    add(V("channelMixer", "Channel Mixer", "Color Correction", {
        F("rr", "Red - Red", 100, -200, 200, "%"), F("rg", "Red - Green", 0, -200, 200, "%"), F("rb", "Red - Blue", 0, -200, 200, "%"),
        F("gr", "Green - Red", 0, -200, 200, "%"), F("gg", "Green - Green", 100, -200, 200, "%"), F("gb", "Green - Blue", 0, -200, 200, "%"),
        F("br", "Blue - Red", 0, -200, 200, "%"), F("bg", "Blue - Green", 0, -200, 200, "%"), F("bb", "Blue - Blue", 100, -200, 200, "%"),
        B("monochrome", "Monochrome", false),
    }));
    add(V("tint", "Tint", "Color Correction", {
        C("black", "Map Black To", Qt::black), C("white", "Map White To", Qt::white), F("amount", "Amount", 100, 0, 100, "%"),
    }));
    add(V("blackWhite", "Black & White", "Color Correction", {}));
    add(V("invert", "Invert", "Channel", { F("blend", "Blend With Original", 0, 0, 100, "%") }));
    add(V("posterize", "Posterize", "Stylize", { I("levels", "Levels", 6, 2, 64) }));
    add(V("threshold", "Threshold", "Stylize", { F("level", "Level", 0.5, 0, 1) }));

    // ---------------- Blur & sharpen ----------------
    add(V("gaussianBlur", "Gaussian Blur", "Blur & Sharpen", {
        F("radius", "Blurriness", 10, 0, 500, "px", 0.1),
        E("dimensions", "Blur Dimensions", 0, {"Horizontal and Vertical", "Horizontal", "Vertical"}),
        B("repeatEdges", "Repeat Edge Pixels", true),
    }));
    add(V("directionalBlur", "Directional Blur", "Blur & Sharpen", {
        A("direction", "Direction", 0), F("length", "Blur Length", 10, 0, 500, "px", 0.1),
    }));
    add(V("zoomBlur", "Radial Zoom Blur", "Blur & Sharpen", {
        P("center", "Center", 0.5, 0.5), F("amount", "Amount", 10, 0, 100, "%", 0.1),
    }));
    add(V("sharpen", "Unsharp Mask", "Blur & Sharpen", {
        F("amount", "Amount", 50, 0, 500, "%"), F("radius", "Radius", 2, 0.1, 50, "px"), F("threshold", "Threshold", 0, 0, 1),
    }));

    // ---------------- Distort / transform ----------------
    add(V("crop", "Crop", "Transform", {
        F("left", "Left", 0, 0, 100, "%", 0.1), F("top", "Top", 0, 0, 100, "%", 0.1),
        F("right", "Right", 0, 0, 100, "%", 0.1), F("bottom", "Bottom", 0, 0, 100, "%", 0.1),
        F("feather", "Edge Feather", 0, 0, 500, "px", 0.1), B("zoom", "Zoom", false),
    }, false));
    add(V("transform", "Transform", "Transform", {
        P("position", "Position", 0.5, 0.5), F("scale", "Scale", 100, 0, 1000, "%", 0.1),
        A("rotation", "Rotation", 0), A("skew", "Skew", 0), A("skewAxis", "Skew Axis", 0),
        P("anchor", "Anchor Point", 0.5, 0.5), F("opacity", "Opacity", 100, 0, 100, "%"),
        F("shutter", "Motion Blur Shutter Angle", 0, 0, 360, "°"),
    }, false));
    add(V("flip", "Flip", "Transform", { B("horizontal", "Horizontal", true), B("vertical", "Vertical", false) }, false));
    add(V("mirror", "Mirror", "Distort", { P("center", "Reflection Center", 0.5, 0.5), A("angle", "Reflection Angle", 0) }));
    add(V("cornerPin", "Corner Pin", "Distort", {
        P("tl", "Upper Left", 0, 0), P("tr", "Upper Right", 1, 0), P("bl", "Lower Left", 0, 1), P("br", "Lower Right", 1, 1),
    }, false));
    add(V("lensDistortion", "Lens Distortion", "Distort", {
        F("curvature", "Curvature", 0, -100, 100), F("zoom", "Zoom", 100, 50, 200, "%"),
    }));
    add(V("wave", "Wave Warp", "Distort", {
        F("height", "Wave Height", 10, 0, 200, "px"), F("width", "Wave Width", 40, 1, 1000, "px"),
        A("direction", "Direction", 90), F("speed", "Wave Speed", 1, -10, 10),
    }));
    add(V("stabilizer", "Warp Stabilizer", "Distort", {
        F("smoothness", "Smoothness", 50, 0, 100, "%"), E("method", "Method", 0, {"Position", "Position, Scale, Rotation"}),
        F("crop", "Auto Crop", 105, 100, 150, "%"), S("analysis", "Analysis", "", ParamType::String),
    }, false));

    // ---------------- Keying ----------------
    add(V("chromaKey", "Ultra Key", "Keying", {
        C("keyColor", "Key Color", QColor(0, 200, 0)),
        F("tolerance", "Tolerance", 30, 0, 100, "%"), F("softness", "Softness", 10, 0, 100, "%"),
        F("choke", "Choke", 0, -100, 100, "%"), F("spill", "Spill Suppression", 50, 0, 100, "%"),
        F("despill", "Desaturate", 25, 0, 100, "%"), B("showMatte", "Show Alpha Channel", false),
    }));
    add(V("lumaKey", "Luma Key", "Keying", {
        F("threshold", "Threshold", 10, 0, 100, "%"), F("cutoff", "Cutoff", 0, 0, 100, "%"),
        B("invert", "Invert", false),
    }));
    add(V("trackMatte", "Track Matte Key", "Keying", {
        I("track", "Matte Track (V#)", 2, 1, 99), E("composite", "Composite Using", 0, {"Matte Alpha", "Matte Luma"}),
        B("reverse", "Reverse", false),
    }, false));

    // ---------------- Stylize / generate ----------------
    add(V("glow", "Glow", "Stylize", {
        F("threshold", "Threshold", 60, 0, 100, "%"), F("radius", "Radius", 20, 0, 300, "px"),
        F("intensity", "Intensity", 1, 0, 10), C("color", "Color", Qt::white),
    }));
    add(V("mosaic", "Mosaic", "Stylize", { I("blocks", "Horizontal Blocks", 40, 1, 2000), B("sharpColors", "Sharp Colors", false) }));
    add(V("filmGrain", "Film Grain", "Stylize", {
        F("amount", "Amount", 20, 0, 100, "%"), F("size", "Grain Size", 1, 0.5, 5), B("monochrome", "Monochrome", true),
    }));
    add(V("vignette", "Vignette", "Stylize", {
        F("amount", "Amount", 50, 0, 100, "%"), F("size", "Size", 70, 0, 150, "%"), F("feather", "Feather", 50, 0, 100, "%"),
        C("color", "Color", Qt::black),
    }));
    add(V("emboss", "Emboss", "Stylize", { A("direction", "Direction", 45), F("relief", "Relief", 2, 0, 10) }));
    add(V("findEdges", "Find Edges", "Stylize", { B("invert", "Invert", false), F("blend", "Blend", 0, 0, 100, "%") }));
    add(V("chromaticAberration", "Chromatic Aberration", "Stylize", { F("amount", "Amount", 2, 0, 50, "px") }));
    add(V("letterbox", "Letterbox", "Transform", {
        F("ratio", "Aspect Ratio", 2.39, 1, 4), C("color", "Color", Qt::black),
    }, false));
    add(V("gradient", "Ramp", "Generate", {
        P("start", "Start of Ramp", 0.5, 0), C("startColor", "Start Color", Qt::black),
        P("end", "End of Ramp", 0.5, 1), C("endColor", "End Color", Qt::white),
        E("shape", "Ramp Shape", 0, {"Linear", "Radial"}), F("blend", "Blend With Original", 0, 0, 100, "%"),
    }));
    add(V("blurRegion", "Blur Region (Faces, Plates)", "Blur & Sharpen", {
        F("radius", "Blurriness", 25, 0, 200, "px"), I("mosaic", "Pixelate Instead (blocks, 0 = off)", 0, 0, 200),
    }));

    // ---------------- Audio ----------------
    {
        EffectDef d = Au("volume", "Volume", "Intrinsic", { F("level", "Level", 0, -96, 15, "dB", 0.1), B("bypass", "Bypass", false) });
        d.intrinsic = true;
        add(d);
    }
    {
        EffectDef d = Au("panner", "Panner", "Intrinsic", { F("pan", "Balance", 0, -100, 100, {}, 1) });
        d.intrinsic = true;
        add(d);
    }
    add(Au("eq", "Parametric Equalizer", "Filter and EQ", {
        F("lowFreq", "Low Shelf Freq", 100, 20, 1000, "Hz", 1), F("lowGain", "Low Shelf Gain", 0, -24, 24, "dB"),
        F("midFreq", "Mid Freq", 1000, 100, 10000, "Hz", 1), F("midGain", "Mid Gain", 0, -24, 24, "dB"), F("midQ", "Mid Q", 1, 0.1, 10),
        F("highFreq", "High Shelf Freq", 8000, 1000, 20000, "Hz", 1), F("highGain", "High Shelf Gain", 0, -24, 24, "dB"),
    }));
    add(Au("highpass", "Highpass", "Filter and EQ", { F("cutoff", "Cutoff", 80, 10, 20000, "Hz", 1) }));
    add(Au("lowpass", "Lowpass", "Filter and EQ", { F("cutoff", "Cutoff", 12000, 10, 20000, "Hz", 1) }));
    add(Au("compressor", "Dynamics Compressor", "Amplitude and Compression", {
        F("threshold", "Threshold", -18, -60, 0, "dB"), F("ratio", "Ratio", 4, 1, 30),
        F("attack", "Attack", 10, 0.1, 200, "ms"), F("release", "Release", 100, 5, 2000, "ms"),
        F("makeup", "Make-up Gain", 0, -12, 24, "dB"),
    }));
    add(Au("limiter", "Hard Limiter", "Amplitude and Compression", {
        F("ceiling", "Maximum Amplitude", -1, -24, 0, "dB"), F("release", "Release", 50, 1, 500, "ms"),
    }));
    add(Au("noiseGate", "Noise Gate", "Amplitude and Compression", {
        F("threshold", "Threshold", -45, -90, 0, "dB"), F("attack", "Attack", 2, 0.1, 100, "ms"), F("release", "Release", 150, 5, 2000, "ms"),
    }));
    add(Au("denoise", "Noise Reduction", "Noise Reduction / Restoration", {
        F("amount", "Amount", 50, 0, 100, "%"), F("sensitivity", "Sensitivity", 50, 0, 100, "%"),
    }));
    add(Au("deesser", "DeEsser", "Noise Reduction / Restoration", {
        F("frequency", "Frequency", 6500, 2000, 12000, "Hz", 1), F("threshold", "Threshold", -25, -60, 0, "dB"),
    }));
    add(Au("reverb", "Studio Reverb", "Reverb", {
        F("room", "Room Size", 50, 0, 100, "%"), F("damping", "Damping", 50, 0, 100, "%"), F("mix", "Dry/Wet", 25, 0, 100, "%"),
    }));
    add(Au("delay", "Delay", "Delay and Echo", {
        F("time", "Delay Time", 250, 1, 2000, "ms"), F("feedback", "Feedback", 30, 0, 95, "%"), F("mix", "Mix", 30, 0, 100, "%"),
    }));
    add(Au("pitch", "Pitch Shifter", "Time and Pitch", { F("semitones", "Semitones", 0, -12, 12, "st", 0.1) }));
    add(Au("channelSwap", "Channel Tools", "Special", {
        E("mode", "Mode", 0, {"Normal", "Swap Left/Right", "Left to Both", "Right to Both", "Mono Mix", "Invert Phase"}),
    }));
    add(Au("ducking", "Auto Ducking", "Special", {
        I("sidechainTrack", "Duck Under Track (A#)", 1, 1, 99), F("amount", "Duck Amount", -15, -40, 0, "dB"),
        F("sensitivity", "Sensitivity", -35, -60, 0, "dB"), F("fade", "Fade Duration", 400, 10, 3000, "ms"),
    }));

    // ---------------- Video transitions ----------------
    add(VT("crossDissolve", "Cross Dissolve", "Dissolve"));
    add(VT("filmDissolve", "Film Dissolve", "Dissolve"));
    add(VT("additiveDissolve", "Additive Dissolve", "Dissolve"));
    add(VT("dipToBlack", "Dip to Black", "Dissolve"));
    add(VT("dipToWhite", "Dip to White", "Dissolve"));
    add(VT("blurDissolve", "Blur Dissolve", "Dissolve", { F("radius", "Blur", 40, 0, 300, "px") }));
    add(VT("wipe", "Wipe", "Wipe", { A("angle", "Angle", 0), F("feather", "Feather", 2, 0, 50, "%") }));
    add(VT("clockWipe", "Clock Wipe", "Wipe", { F("feather", "Feather", 1, 0, 50, "%") }));
    add(VT("irisRound", "Iris Round", "Iris", { P("center", "Center", 0.5, 0.5), F("feather", "Feather", 2, 0, 50, "%") }));
    add(VT("irisBox", "Iris Box", "Iris", { P("center", "Center", 0.5, 0.5), F("feather", "Feather", 1, 0, 50, "%") }));
    add(VT("barnDoor", "Barn Door", "Wipe", { B("vertical", "Vertical", false) }));
    add(VT("push", "Push", "Slide", { E("direction", "Direction", 0, {"Left", "Right", "Up", "Down"}) }));
    add(VT("slide", "Slide", "Slide", { E("direction", "Direction", 0, {"Left", "Right", "Up", "Down"}) }));
    add(VT("zoom", "Cross Zoom", "Zoom", { F("strength", "Strength", 3, 1, 10) }));
    add(VT("spin", "Spin", "3D Motion", {}));
    add(VT("whip", "Whip Pan", "Slide", { E("direction", "Direction", 0, {"Left", "Right", "Up", "Down"}) }));
    add(VT("glitch", "Glitch", "Stylize", { F("intensity", "Intensity", 50, 0, 100, "%") }));
    add(VT("lumaFade", "Luma Fade", "Dissolve", { F("softness", "Softness", 10, 0, 100, "%"), B("invert", "Invert", false) }));

    // ---------------- Audio transitions ----------------
    add(AT("constantPower", "Constant Power"));
    add(AT("constantGain", "Constant Gain"));
    add(AT("exponentialFade", "Exponential Fade"));
}

} // namespace rc
