#include "Title.h"
#include <QCryptographicHash>
#include <QFontMetricsF>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QTextLayout>
#include <cmath>

namespace rc {

namespace {

struct Anim {
    double opacity = 1, dx = 0, dy = 0, scale = 1, reveal = 1;   // reveal: fraction of chars / wipe
    bool active = false;
};

double easeOut(double u) { return 1 - std::pow(1 - std::clamp(u, 0.0, 1.0), 3); }
double backOut(double u)
{
    u = std::clamp(u, 0.0, 1.0);
    const double c1 = 1.70158, c3 = c1 + 1;
    return 1 + c3 * std::pow(u - 1, 3) + c1 * std::pow(u - 1, 2);
}

void applyPhase(Anim &a, const QString &type, double u, bool in)
{
    // u: 0 = fully hidden, 1 = fully visible
    if (type.isEmpty() || type == "none")
        return;
    a.active = a.active || u < 1;
    const double e = easeOut(u);
    if (type == "fade") a.opacity *= e;
    else if (type == "slideUp") { a.dy += (1 - e) * 0.06; a.opacity *= e; }
    else if (type == "slideDown") { a.dy -= (1 - e) * 0.06; a.opacity *= e; }
    else if (type == "slideLeft") { a.dx += (1 - e) * 0.08; a.opacity *= e; }
    else if (type == "slideRight") { a.dx -= (1 - e) * 0.08; a.opacity *= e; }
    else if (type == "pop") { a.scale *= in ? std::max(0.0, backOut(u)) : e; a.opacity *= std::min(1.0, u * 3); }
    else if (type == "zoom") { a.scale *= 0.5 + 0.5 * e; a.opacity *= e; }
    else if (type == "typewriter") a.reveal = std::min(a.reveal, u);
    else if (type == "wipe") a.reveal = std::min(a.reveal, e);
    else if (type == "blur") a.opacity *= e;
}

Anim layerAnim(const QJsonObject &layer, Ticks local, Ticks duration)
{
    Anim a;
    QJsonObject an = layer["anim"].toObject();
    const double t = ticksToSeconds(local);
    const double total = ticksToSeconds(duration);
    const double inDur = an["inDuration"].toDouble(0.5);
    const double outDur = an["outDuration"].toDouble(0.5);
    const double delay = an["delay"].toDouble(0);
    QString in = an["in"].toString(), out = an["out"].toString();
    if (!in.isEmpty() && in != "none")
        applyPhase(a, in, inDur > 0 ? (t - delay) / inDur : 1, true);
    if (!out.isEmpty() && out != "none" && total > 0)
        applyPhase(a, out, outDur > 0 ? (total - t) / outDur : 1, false);
    return a;
}

QColor col(const QJsonValue &v, QColor def)
{
    QString s = v.toString();
    if (s.isEmpty())
        return def;
    QColor c(s);
    return c.isValid() ? c : def;
}

void drawText(QPainter &p, const QJsonObject &L, int w, int h, const Anim &a)
{
    QString text = L["text"].toString();
    if (L["uppercase"].toBool())
        text = text.toUpper();
    if (a.reveal < 1) {
        int n = (int)std::floor(text.size() * a.reveal);
        text = text.left(n);
    }
    const double sizePx = std::max(1.0, L["size"].toDouble(0.07) * h);
    QFont font(L["font"].toString("Inter"));
    font.setPixelSize(std::max(1, (int)std::lround(sizePx)));
    font.setBold(L["bold"].toBool(true));
    font.setItalic(L["italic"].toBool(false));
    if (L.contains("weight"))
        font.setWeight(QFont::Weight(L["weight"].toInt(700)));
    font.setLetterSpacing(QFont::PercentageSpacing, 100 + L["tracking"].toDouble(0));
    QFontMetricsF fm(font);
    const double lineH = fm.height() * L["lineHeight"].toDouble(1.0);
    QStringList lines = text.split('\n');
    double maxW = 0;
    for (const auto &l : lines)
        maxW = std::max(maxW, fm.horizontalAdvance(l));
    const double blockH = lineH * lines.size();
    const QString align = L["align"].toString("center");
    const double x = (L["x"].toDouble(0.5) + a.dx) * w;
    const double y = (L["y"].toDouble(0.5) + a.dy) * h;

    p.save();
    p.translate(x, y);
    p.rotate(L["rotation"].toDouble(0));
    p.scale(a.scale, a.scale);
    p.setOpacity(a.opacity * L["opacity"].toDouble(1.0));
    double left = align == "left" ? 0 : align == "right" ? -maxW : -maxW / 2;
    double top = -blockH / 2;

    QJsonObject bg = L["background"].toObject();
    if (!bg.isEmpty() && bg["enabled"].toBool(true)) {
        const double pad = bg["padding"].toDouble(0.3) * sizePx;
        QRectF r(left - pad, top - pad * 0.5, maxW + 2 * pad, blockH + pad);
        p.setPen(Qt::NoPen);
        p.setBrush(col(bg["color"], QColor(0, 0, 0, 160)));
        double rad = bg["radius"].toDouble(0.15) * sizePx;
        p.drawRoundedRect(r, rad, rad);
    }

    QPainterPath path;
    for (int i = 0; i < lines.size(); ++i) {
        double lw = fm.horizontalAdvance(lines[i]);
        double lx = align == "left" ? left : align == "right" ? -lw : -lw / 2;
        double ly = top + i * lineH + fm.ascent() + (lineH - fm.height()) / 2;
        path.addText(QPointF(lx, ly), font, lines[i]);
    }
    p.setRenderHint(QPainter::Antialiasing);
    QJsonObject sh = L["shadow"].toObject();
    if (!sh.isEmpty() && sh["enabled"].toBool(true)) {
        QColor sc = col(sh["color"], QColor(0, 0, 0, 150));
        double dist = sh["distance"].toDouble(0.06) * sizePx;
        double ang = sh["angle"].toDouble(135) * M_PI / 180.0;
        double soft = sh["softness"].toDouble(0.08) * sizePx;
        // Cheap soft shadow: several offset passes.
        int passes = soft > 1 ? 6 : 1;
        for (int k = 0; k < passes; ++k) {
            QColor c = sc;
            c.setAlphaF(sc.alphaF() / passes * 1.5);
            double r = passes > 1 ? soft * (double(k) / (passes - 1) - 0.5) : 0;
            p.fillPath(path.translated(std::cos(ang) * dist + r, std::sin(ang) * dist + r), c);
        }
    }
    const double ow = L["outlineWidth"].toDouble(0) * sizePx;
    if (ow > 0) {
        QPen pen(col(L["outlineColor"], Qt::black), ow * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.strokePath(path, pen);
    }
    // Fill: solid or gradient.
    QJsonObject grad = L["gradient"].toObject();
    if (!grad.isEmpty()) {
        QLinearGradient g(0, top, 0, top + blockH);
        g.setColorAt(0, col(grad["from"], Qt::white));
        g.setColorAt(1, col(grad["to"], Qt::gray));
        p.fillPath(path, g);
    } else {
        p.fillPath(path, col(L["color"], Qt::white));
    }
    p.restore();
}

void drawShape(QPainter &p, const QJsonObject &L, int w, int h, const Anim &a, bool ellipse)
{
    const double x = (L["x"].toDouble(0.5) + a.dx) * w;
    const double y = (L["y"].toDouble(0.5) + a.dy) * h;
    double rw = L["w"].toDouble(0.3) * w;
    double rh = L["h"].toDouble(0.1) * h;
    p.save();
    p.translate(x, y);
    p.rotate(L["rotation"].toDouble(0));
    p.scale(a.scale, a.scale);
    p.setOpacity(a.opacity * L["opacity"].toDouble(1.0));
    p.setRenderHint(QPainter::Antialiasing);
    QRectF r(-rw / 2, -rh / 2, rw * (a.reveal < 1 ? a.reveal : 1), rh);
    if (L["anchor"].toString() == "left")
        r.moveLeft(0);
    double sw = L["strokeWidth"].toDouble(0) * h;
    p.setPen(sw > 0 ? QPen(col(L["strokeColor"], Qt::white), sw) : QPen(Qt::NoPen));
    QJsonObject grad = L["gradient"].toObject();
    if (!grad.isEmpty()) {
        QLinearGradient g(r.topLeft(), r.topRight());
        g.setColorAt(0, col(grad["from"], Qt::white));
        g.setColorAt(1, col(grad["to"], Qt::gray));
        p.setBrush(g);
    } else {
        p.setBrush(col(L["color"], QColor(30, 30, 30, 200)));
    }
    if (ellipse)
        p.drawEllipse(r);
    else {
        double rad = L["radius"].toDouble(0) * h;
        p.drawRoundedRect(r, rad, rad);
    }
    p.restore();
}

} // namespace

QString TitleRenderer::key(const QJsonObject &doc, int w, int h, Ticks local, Ticks duration)
{
    QByteArray base = QJsonDocument(doc).toJson(QJsonDocument::Compact);
    QString k = QString::fromLatin1(QCryptographicHash::hash(base, QCryptographicHash::Md5).toHex()) +
                QString("_%1x%2").arg(w).arg(h);
    // Animated layers change with time.
    QStringList phases;
    for (const auto &lv : doc["layers"].toArray()) {
        Anim a = layerAnim(lv.toObject(), local, duration);
        if (a.active)
            phases << QString("%1,%2,%3,%4,%5").arg(a.opacity, 0, 'f', 3).arg(a.dx, 0, 'f', 4).arg(a.dy, 0, 'f', 4).arg(a.scale, 0, 'f', 3).arg(a.reveal, 0, 'f', 3);
    }
    if (!phases.isEmpty())
        k += "_" + phases.join(";");
    return k;
}

QImage TitleRenderer::render(const QJsonObject &doc, int w, int h, Ticks local, Ticks duration)
{
    QImage img(std::max(1, w), std::max(1, h), QImage::Format_RGBA8888_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    for (const auto &lv : doc["layers"].toArray()) {
        QJsonObject L = lv.toObject();
        if (!L["visible"].toBool(true))
            continue;
        Anim a = layerAnim(L, local, duration);
        const QString type = L["type"].toString("text");
        if (type == "text")
            drawText(p, L, w, h, a);
        else if (type == "rect")
            drawShape(p, L, w, h, a, false);
        else if (type == "ellipse")
            drawShape(p, L, w, h, a, true);
        else if (type == "image") {
            QImage im(L["path"].toString());
            if (!im.isNull()) {
                QRectF r((L["x"].toDouble(0.5) - L["w"].toDouble(0.2) / 2 + a.dx) * w, (L["y"].toDouble(0.5) - L["h"].toDouble(0.2) / 2 + a.dy) * h,
                         L["w"].toDouble(0.2) * w, L["h"].toDouble(0.2) * h);
                p.setOpacity(a.opacity);
                p.drawImage(r, im);
                p.setOpacity(1);
            }
        }
    }
    return img;
}

QImage TitleRenderer::renderCaption(const CaptionStyle &st, const QString &textIn, int w, int h, int highlightWord)
{
    QImage img(std::max(1, w), std::max(1, h), QImage::Format_RGBA8888_Premultiplied);
    img.fill(Qt::transparent);
    QString text = st.uppercase ? textIn.toUpper() : textIn;
    if (text.trimmed().isEmpty())
        return img;
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    QFont font(st.font);
    font.setPixelSize(std::max(4, int(st.size * h)));
    font.setBold(st.bold);
    QFontMetricsF fm(font);
    // Word wrap to maxCharsPerLine and frame width.
    QStringList words = text.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    QStringList lines;
    QList<QList<int>> lineWords;
    QString cur;
    QList<int> curIdx;
    const double maxW = w * 0.86;
    for (int i = 0; i < words.size(); ++i) {
        QString cand = cur.isEmpty() ? words[i] : cur + " " + words[i];
        if (!cur.isEmpty() && (cand.size() > st.maxCharsPerLine || fm.horizontalAdvance(cand) > maxW)) {
            lines << cur;
            lineWords << curIdx;
            cur = words[i];
            curIdx = {i};
        } else {
            cur = cand;
            curIdx << i;
        }
    }
    if (!cur.isEmpty()) {
        lines << cur;
        lineWords << curIdx;
    }
    while (lines.size() > std::max(1, st.maxLines) * 2)
        lines.removeFirst();
    const double lineH = fm.height() * 1.1;
    const double blockH = lineH * lines.size();
    double baseY = st.positionY * h - blockH / 2;
    for (int li = 0; li < lines.size(); ++li) {
        const double lw = fm.horizontalAdvance(lines[li]);
        double x = (w - lw) / 2;
        const double y = baseY + li * lineH + fm.ascent();
        if (st.background.alpha() > 0) {
            QRectF r(x - fm.height() * 0.25, baseY + li * lineH, lw + fm.height() * 0.5, lineH);
            p.setPen(Qt::NoPen);
            p.setBrush(st.background);
            p.drawRoundedRect(r, fm.height() * 0.15, fm.height() * 0.15);
        }
        // Draw word by word for highlight support.
        const QStringList lws = lines[li].split(' ');
        for (int wi = 0; wi < lws.size(); ++wi) {
            QPainterPath path;
            path.addText(QPointF(x, y), font, lws[wi]);
            if (st.outlineWidth > 0)
                p.strokePath(path, QPen(st.outline, st.outlineWidth * font.pixelSize() * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            int globalIdx = (li < lineWords.size() && wi < lineWords[li].size()) ? lineWords[li][wi] : -1;
            QColor c = (st.wordHighlight && globalIdx == highlightWord) ? st.highlight : st.color;
            p.fillPath(path, c);
            x += fm.horizontalAdvance(lws[wi] + " ");
        }
    }
    return img;
}

QStringList TitleRenderer::templateNames()
{
    return {"Basic Title", "Lower Third", "Lower Third (Bar)", "Subscribe Badge", "Big Center Title",
            "Chapter Card", "Quote", "Credits Roll", "Social Hook", "Location Tag"};
}

QJsonObject TitleRenderer::textDoc(const QString &text)
{
    QJsonObject t{{"type", "text"}, {"text", text}, {"font", "Inter"}, {"size", 0.08}, {"color", "#ffffff"},
                  {"bold", true}, {"x", 0.5}, {"y", 0.5}, {"align", "center"}};
    return QJsonObject{{"layers", QJsonArray{t}}};
}

QJsonObject TitleRenderer::templateDoc(const QString &name)
{
    auto text = [](const QString &s, double x, double y, double size, const QString &align = "center") {
        return QJsonObject{{"type", "text"}, {"text", s}, {"font", "Inter"}, {"size", size}, {"color", "#ffffff"},
                           {"bold", true}, {"x", x}, {"y", y}, {"align", align}};
    };
    QJsonArray layers;
    if (name == "Lower Third") {
        QJsonObject n = text("Firstname Lastname", 0.08, 0.78, 0.05, "left");
        n["anim"] = QJsonObject{{"in", "slideRight"}, {"out", "fade"}, {"inDuration", 0.5}, {"outDuration", 0.4}};
        n["shadow"] = QJsonObject{{"enabled", true}};
        QJsonObject r = text("Role / Title", 0.08, 0.845, 0.032, "left");
        r["bold"] = false;
        r["color"] = "#d8d8e0";
        r["anim"] = QJsonObject{{"in", "slideRight"}, {"out", "fade"}, {"delay", 0.15}};
        layers = {n, r};
    } else if (name == "Lower Third (Bar)") {
        QJsonObject bar{{"type", "rect"}, {"x", 0.06}, {"y", 0.8}, {"w", 0.42}, {"h", 0.11}, {"color", "#e6c23c"}, {"anchor", "left"},
                        {"anim", QJsonObject{{"in", "wipe"}, {"out", "fade"}, {"inDuration", 0.45}}}};
        QJsonObject n = text("Firstname Lastname", 0.08, 0.785, 0.045, "left");
        n["color"] = "#111111";
        n["anim"] = QJsonObject{{"in", "fade"}, {"delay", 0.3}, {"out", "fade"}};
        QJsonObject r = text("Role / Title", 0.08, 0.835, 0.03, "left");
        r["color"] = "#222222";
        r["bold"] = false;
        r["anim"] = QJsonObject{{"in", "fade"}, {"delay", 0.4}, {"out", "fade"}};
        layers = {bar, n, r};
    } else if (name == "Subscribe Badge") {
        QJsonObject b{{"type", "rect"}, {"x", 0.85}, {"y", 0.88}, {"w", 0.18}, {"h", 0.07}, {"color", "#d84a4a"}, {"radius", 0.012},
                      {"anim", QJsonObject{{"in", "pop"}, {"out", "zoom"}}}};
        QJsonObject t = text("SUBSCRIBE", 0.85, 0.88, 0.035);
        t["anim"] = QJsonObject{{"in", "pop"}, {"out", "zoom"}};
        layers = {b, t};
    } else if (name == "Big Center Title") {
        QJsonObject t = text("YOUR TITLE", 0.5, 0.5, 0.14);
        t["anim"] = QJsonObject{{"in", "zoom"}, {"out", "fade"}, {"inDuration", 0.6}};
        t["tracking"] = 8;
        layers = {t};
    } else if (name == "Chapter Card") {
        QJsonObject n = text("CHAPTER 1", 0.5, 0.44, 0.035);
        n["color"] = "#e6c23c";
        n["tracking"] = 30;
        n["anim"] = QJsonObject{{"in", "fade"}, {"out", "fade"}};
        QJsonObject t = text("The Beginning", 0.5, 0.52, 0.09);
        t["anim"] = QJsonObject{{"in", "slideUp"}, {"out", "fade"}, {"delay", 0.2}};
        layers = {n, t};
    } else if (name == "Quote") {
        QJsonObject t = text("\"A memorable quote\non two lines.\"", 0.5, 0.48, 0.06);
        t["italic"] = true;
        t["bold"] = false;
        t["anim"] = QJsonObject{{"in", "typewriter"}, {"inDuration", 2.0}, {"out", "fade"}};
        QJsonObject a = text("- Author", 0.5, 0.62, 0.035);
        a["anim"] = QJsonObject{{"in", "fade"}, {"delay", 2.0}, {"out", "fade"}};
        layers = {t, a};
    } else if (name == "Credits Roll") {
        QJsonObject t = text("Directed by\nName\n\nEdited with\nRaccord", 0.5, 0.5, 0.045);
        t["bold"] = false;
        t["lineHeight"] = 1.3;
        layers = {t};
    } else if (name == "Social Hook") {
        QJsonObject t = text("WAIT FOR IT...", 0.5, 0.2, 0.075);
        t["outlineWidth"] = 0.08;
        t["anim"] = QJsonObject{{"in", "pop"}, {"out", "fade"}};
        layers = {t};
    } else if (name == "Location Tag") {
        QJsonObject t = text("PARIS, FRANCE", 0.06, 0.1, 0.035, "left");
        t["tracking"] = 20;
        t["background"] = QJsonObject{{"enabled", true}, {"color", "#99000000"}, {"padding", 0.5}};
        t["anim"] = QJsonObject{{"in", "typewriter"}, {"inDuration", 0.8}, {"out", "fade"}};
        layers = {t};
    } else {
        QJsonObject t = text("Title", 0.5, 0.5, 0.08);
        t["shadow"] = QJsonObject{{"enabled", true}};
        layers = {t};
    }
    return QJsonObject{{"layers", layers}, {"template", name}};
}

} // namespace rc
