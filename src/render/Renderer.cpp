#include "Renderer.h"
#include "Shaders.h"
#include "Title.h"
#include "core/Edit.h"
#include "media/MediaPool.h"
#include <QPainter>
#include <QJsonDocument>
#include <QRegularExpression>
#include <cmath>

namespace rc {

Renderer::Renderer()
{
    m_gl = std::make_unique<GL>();
    m_ok = m_gl->f != nullptr;
    if (!m_ok)
        m_error = m_gl->lastError();
}

Renderer::~Renderer()
{
    if (!m_gl || !m_gl->f)
        return;
    releaseCaches();
    for (GLuint &t : m_planes)
        m_gl->deleteTexture(t);
}

void Renderer::releaseCaches()
{
    m_converted.clear();
    for (auto &l : m_luts)
        m_gl->deleteTexture(l.tex);
    m_luts.clear();
    for (auto t : m_curves)
        m_gl->deleteTexture(t);
    m_curves.clear();
    for (auto t : m_imageTex)
        m_gl->deleteTexture(t);
    m_imageTex.clear();
    m_imageTexOrder.clear();
    m_gl->trimPool();
}

GLuint Renderer::cachedImageTexture(const QString &key, const std::function<QImage()> &make)
{
    auto it = m_imageTex.find(key);
    if (it != m_imageTex.end())
        return it.value();
    QImage img = make();
    GLuint t = m_gl->uploadImage(img);
    m_imageTex.insert(key, t);
    m_imageTexOrder.push_back(key);
    while (m_imageTexOrder.size() > 48) {
        m_gl->deleteTexture(m_imageTex.take(m_imageTexOrder.front()));
        m_imageTexOrder.erase(m_imageTexOrder.begin());
    }
    return t;
}

// ---------------------------------------------------------------------------
// Frame upload and conversion

static void yuvMatrix(const VideoFrame &f, float m[9], float off[3])
{
    double kr = 0.2126, kb = 0.0722;
    if (f.matrix == YuvMatrix::Bt601) { kr = 0.299; kb = 0.114; }
    else if (f.matrix == YuvMatrix::Bt2020) { kr = 0.2627; kb = 0.0593; }
    const double kg = 1 - kr - kb;
    const int n = std::min(f.bitDepth, 16);
    const double maxCode = double((1 << n) - 1);
    const double unit = double(1 << (n - 8));
    double yOff, yScale, cOff, cScale;
    if (f.fullRange) {
        yOff = 0; yScale = 1; cOff = double(1 << (n - 1)) / maxCode; cScale = 1;
    } else {
        yOff = 16 * unit / maxCode; yScale = maxCode / (219 * unit);
        cOff = 128 * unit / maxCode; cScale = maxCode / (224 * unit);
    }
    // rgb = M * (yuv - off), column major.
    const double crR = 2 * (1 - kr), cbB = 2 * (1 - kb);
    const double cbG = -2 * kb * (1 - kb) / kg, crG = -2 * kr * (1 - kr) / kg;
    // columns: Y, Cb, Cr
    m[0] = float(yScale); m[1] = float(yScale); m[2] = float(yScale);
    m[3] = 0; m[4] = float(cbG * cScale); m[5] = float(cbB * cScale);
    m[6] = float(crR * cScale); m[7] = float(crG * cScale); m[8] = 0;
    off[0] = float(yOff); off[1] = float(cOff); off[2] = float(cOff);
}

SurfaceRef Renderer::convertFrame(const VideoFramePtr &frame, double scale, int alphaMode)
{
    if (!frame || !frame->valid())
        return {};
    for (auto &c : m_converted)
        if (c.frame.lock() == frame && std::abs(c.scale - scale) < 1e-6 && c.alphaMode == alphaMode)
            return c.surface;
    auto *f = m_gl->f;
    const VideoFrame &vf = *frame;
    const int planes = vf.planeCount();
    const int bpc = vf.bytesPerComponent();
    for (int p = 0; p < planes; ++p) {
        int w = vf.width, h = vf.height, comps = 1;
        if (vf.layout == PixelLayout::Rgba) comps = 4;
        if ((vf.layout == PixelLayout::Planar && (p == 1 || p == 2)) || (vf.layout == PixelLayout::SemiPlanar && p == 1)) {
            w = (vf.width + (1 << vf.chromaShiftX) - 1) >> vf.chromaShiftX;
            h = (vf.height + (1 << vf.chromaShiftY) - 1) >> vf.chromaShiftY;
        }
        if (vf.layout == PixelLayout::SemiPlanar && p == 1) comps = 2;
        GLenum internal, fmt = comps == 1 ? GL_RED : comps == 2 ? GL_RG : GL_RGBA;
        if (bpc == 1) internal = comps == 1 ? GL_R8 : comps == 2 ? GL_RG8 : GL_RGBA8;
        else internal = comps == 1 ? GL_R16 : comps == 2 ? GL_RG16 : GL_RGBA16;
        const GLenum type = bpc == 1 ? GL_UNSIGNED_BYTE : GL_UNSIGNED_SHORT;
        if (!m_planes[p])
            f->glGenTextures(1, &m_planes[p]);
        f->glBindTexture(GL_TEXTURE_2D, m_planes[p]);
        f->glPixelStorei(GL_UNPACK_ROW_LENGTH, vf.linesize[p] / (bpc * comps));
        if (m_planeW[p] != w || m_planeH[p] != h || m_planeFmt[p] != internal) {
            f->glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, fmt, type, vf.data[p]);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            m_planeW[p] = w;
            m_planeH[p] = h;
            m_planeFmt[p] = internal;
        } else {
            f->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, type, vf.data[p]);
        }
        f->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    const bool rot90 = vf.rotation == 90 || vf.rotation == 270;
    int outW = rot90 ? vf.height : vf.width;
    int outH = rot90 ? vf.width : vf.height;
    outW = std::max(1, int(std::lround(outW * vf.pixelAspect * scale)));
    outH = std::max(1, int(std::lround(outH * scale)));
    SurfaceRef out = m_gl->surface(outW, outH);
    m_gl->use(m_gl->program("convert", shaders::convert));
    m_gl->target(out);
    for (int p = 0; p < 4; ++p)
        m_gl->bindTexture(QByteArray("uP" + QByteArray::number(p)).constData(), p, p < planes ? m_planes[p] : m_planes[0]);
    int layout = vf.layout == PixelLayout::Planar ? 0 : vf.layout == PixelLayout::SemiPlanar ? 1 : vf.layout == PixelLayout::Rgba ? 2 : 3;
    m_gl->set("uLayout", layout);
    float sc = 1.0f;
    if (bpc == 2 && vf.layout != PixelLayout::Rgba) {
        if (vf.msbAligned)
            sc = float(65535.0 / (double((1 << vf.bitDepth) - 1) * double(1 << (16 - vf.bitDepth))));
        else
            sc = float(65535.0 / double((1 << vf.bitDepth) - 1));
    }
    m_gl->set("uScale", sc);
    float m[9], off[3];
    yuvMatrix(vf, m, off);
    m_gl->setMat3("uYuv", m);
    m_gl->set("uOffset", off[0], off[1], off[2]);
    bool alpha = vf.hasAlpha && alphaMode != 1;
    m_gl->set("uHasAlpha", alpha ? 1 : 0);
    m_gl->set("uBgr", vf.bgr ? 1 : 0);
    m_gl->set("uRot", vf.rotation);
    // Decoded RGBA from FFmpeg/QImage is straight alpha unless told otherwise.
    m_gl->set("uStraight", alphaMode == 2 ? 0 : 1);
    m_gl->set("uTransfer", vf.transfer == TransferFn::Pq ? 1 : vf.transfer == TransferFn::Hlg ? 2 : 0);
    m_gl->set("uGamut2020", vf.matrix == YuvMatrix::Bt2020 ? 1 : 0);
    m_gl->drawQuad();
    m_converted.push_back({frame, out, scale, alphaMode});
    if (m_converted.size() > 12)
        m_converted.erase(m_converted.begin());
    return out;
}

// ---------------------------------------------------------------------------

SurfaceRef Renderer::blend(const SurfaceRef &base, const SurfaceRef &layer, BlendMode mode, float opacity)
{
    SurfaceRef out = m_gl->surface(base->w, base->h);
    m_gl->use(m_gl->program("blend", shaders::blend));
    m_gl->target(out);
    m_gl->bindTexture("uBase", 0, base->tex);
    m_gl->bindTexture("uLayer", 1, layer->tex);
    m_gl->set("uMode", int(mode));
    m_gl->set("uOpacity", opacity);
    m_gl->drawQuad();
    return out;
}

SurfaceRef Renderer::blur(const SurfaceRef &srcIn, float radiusPx, int dims, bool repeatEdges)
{
    if (radiusPx < 0.3f)
        return srcIn;
    // Downscale for large radii so the kernel stays small.
    SurfaceRef src = srcIn;
    int levels = 0;
    float r = radiusPx;
    while (r > 24.f && src->w > 8 && src->h > 8) {
        SurfaceRef half = m_gl->surface(std::max(1, src->w / 2), std::max(1, src->h / 2));
        m_gl->use(m_gl->program("downscale", shaders::downscale));
        m_gl->target(half);
        m_gl->bindTexture("uTex", 0, src->tex);
        m_gl->set("uTexel", 1.f / src->w, 1.f / src->h);
        m_gl->drawQuad();
        src = half;
        r *= 0.5f;
        ++levels;
    }
    const float sigma = std::max(0.5f, r / 2.f);
    const int taps = std::min(48, int(std::ceil(sigma * 3)));
    Program *p = m_gl->program("blur1d", shaders::blur1d);
    if (dims != 2) {
        SurfaceRef h = m_gl->surface(src->w, src->h);
        m_gl->use(p);
        m_gl->target(h);
        m_gl->bindTexture("uTex", 0, src->tex);
        m_gl->set("uStep", 1.f / src->w, 0.f);
        m_gl->set("uSigma", sigma);
        m_gl->set("uRadius", taps);
        m_gl->set("uRepeat", repeatEdges ? 1 : 0);
        m_gl->drawQuad();
        src = h;
    }
    if (dims != 1) {
        SurfaceRef v = m_gl->surface(src->w, src->h);
        m_gl->use(p);
        m_gl->target(v);
        m_gl->bindTexture("uTex", 0, src->tex);
        m_gl->set("uStep", 0.f, 1.f / src->h);
        m_gl->set("uSigma", sigma);
        m_gl->set("uRadius", taps);
        m_gl->set("uRepeat", repeatEdges ? 1 : 0);
        m_gl->drawQuad();
        src = v;
    }
    if (levels > 0) {
        SurfaceRef up = m_gl->surface(srcIn->w, srcIn->h);
        m_gl->blit(src, up);
        src = up;
    }
    return src;
}

SurfaceRef Renderer::downscale(const SurfaceRef &s, int maxW)
{
    SurfaceRef cur = s;
    while (cur->w > maxW * 2) {
        SurfaceRef half = m_gl->surface(std::max(1, cur->w / 2), std::max(1, cur->h / 2));
        m_gl->use(m_gl->program("downscale", shaders::downscale));
        m_gl->target(half);
        m_gl->bindTexture("uTex", 0, cur->tex);
        m_gl->set("uTexel", 1.f / cur->w, 1.f / cur->h);
        m_gl->drawQuad();
        cur = half;
    }
    if (cur->w > maxW) {
        SurfaceRef out = m_gl->surface(maxW, std::max(1, int(int64_t(cur->h) * maxW / cur->w)));
        m_gl->blit(cur, out);
        cur = out;
    }
    return cur;
}

// ---------------------------------------------------------------------------
// Sources

SurfaceRef Renderer::generator(Ctx &c, const Clip &clip, Ticks local, int w, int h)
{
    SurfaceRef out = m_gl->surface(w, h);
    m_gl->target(out);
    switch (clip.source) {
    case ClipSource::Solid: {
        QColor col(clip.gen["color"].toString("#000000"));
        m_gl->use(m_gl->program("solid", shaders::solid));
        m_gl->set("uColor", float(col.redF()), float(col.greenF()), float(col.blueF()), float(col.alphaF()));
        m_gl->drawQuad();
        break;
    }
    case ClipSource::Bars:
        m_gl->use(m_gl->program("bars", shaders::bars));
        m_gl->drawQuad();
        break;
    case ClipSource::Countdown: {
        const FrameRate fps = c.seq->fps;
        const double secs = ticksToSeconds(local);
        const int number = std::max(2, 8 - int(secs));
        const double frac = secs - std::floor(secs);
        QString key = QString("countdown_%1x%2_%3_%4").arg(w).arg(h).arg(number).arg(int(frac * fps.fps()));
        GLuint tex = cachedImageTexture(key, [&] {
            QImage img(w, h, QImage::Format_RGBA8888_Premultiplied);
            img.fill(QColor(30, 30, 30));
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing);
            QPointF ctr(w / 2.0, h / 2.0);
            double R = h * 0.42;
            p.setBrush(QColor(70, 70, 70));
            p.setPen(Qt::NoPen);
            p.drawPie(QRectF(ctr.x() - R * 2, ctr.y() - R * 2, R * 4, R * 4), 90 * 16, -int(frac * 360 * 16));
            p.setPen(QPen(QColor(220, 220, 220), h * 0.006));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(ctr, R, R);
            p.drawEllipse(ctr, R * 0.85, R * 0.85);
            p.drawLine(QPointF(0, ctr.y()), QPointF(w, ctr.y()));
            p.drawLine(QPointF(ctr.x(), 0), QPointF(ctr.x(), h));
            QFont f("Inter");
            f.setPixelSize(int(h * 0.5));
            f.setBold(true);
            p.setFont(f);
            p.setPen(Qt::white);
            p.drawText(QRectF(0, 0, w, h), Qt::AlignCenter, QString::number(number));
            return img;
        });
        m_gl->use(m_gl->program("blit_img", R"(#version 330 core
in vec2 vUv; out vec4 o; uniform sampler2D uTex; void main(){ o = texture(uTex, vUv); })"));
        m_gl->bindTexture("uTex", 0, tex);
        m_gl->drawQuad();
        break;
    }
    case ClipSource::Title: {
        QString key = TitleRenderer::key(clip.gen, w, h, local, clip.duration);
        GLuint tex = cachedImageTexture("title_" + key, [&] { return TitleRenderer::render(clip.gen, w, h, local, clip.duration); });
        m_gl->use(m_gl->program("blit_img", R"(#version 330 core
in vec2 vUv; out vec4 o; uniform sampler2D uTex; void main(){ o = texture(uTex, vUv); })"));
        m_gl->bindTexture("uTex", 0, tex);
        m_gl->drawQuad();
        break;
    }
    case ClipSource::Transparent:
    default:
        m_gl->clear(0, 0, 0, 0);
        break;
    }
    return out;
}

SurfaceRef Renderer::clipSource(Ctx &c, const Clip &clip, Ticks t, double clipScale, int *nativeW, int *nativeH, const SurfaceRef &below)
{
    const Ticks local = t - clip.start;
    const Sequence &seq = *c.seq;
    switch (clip.source) {
    case ClipSource::Media: {
        const MediaItem *m = c.project->mediaItem(clip.mediaId);
        if (!m || !m->info.hasVideo)
            return {};
        const bool rot90 = m->info.rotation == 90 || m->info.rotation == 270;
        *nativeW = int(std::lround((rot90 ? m->info.height : m->info.width) * m->info.pixelAspect));
        *nativeH = rot90 ? m->info.width : m->info.height;
        QString path = m->path;
        if (c.opt.useProxies && !c.opt.exporting && c.project->settings.useProxies && !m->proxyPath.isEmpty())
            path = m->proxyPath;
        VideoFramePtr frame;
        if (m->info.isStill)
            frame = MediaPool::instance().image(path);
        else
            frame = MediaPool::instance().videoFrame(path, clip.sourceAt(t), m->fps());
        if (!frame) {
            ++m_misses;
            return {};
        }
        const int decodedH = (frame->rotation == 90 || frame->rotation == 270) ? frame->width : frame->height;
        const double scale = std::min(1.0, clipScale * double(*nativeH) / std::max(1, decodedH));
        // Scale relative to actual decoded size (proxy or original).
        SurfaceRef s = convertFrame(frame, scale, m->alphaMode);
        return s;
    }
    case ClipSource::Sequence: {
        const Sequence *nested = c.project->sequence(clip.mediaId);
        if (!nested || c.depth >= c.opt.maxNesting || nested->id == seq.id)
            return {};
        *nativeW = nested->width;
        *nativeH = nested->height;
        Ctx n = c;
        n.seq = nested;
        n.depth = c.depth + 1;
        n.outW = std::max(1, int(nested->width * clipScale));
        n.outH = std::max(1, int(nested->height * clipScale));
        n.renderScale = clipScale;
        n.opt.showCaptions = false;
        return composite(n, clip.sourceAt(t));
    }
    case ClipSource::Adjustment:
        *nativeW = seq.width;
        *nativeH = seq.height;
        return below;
    default:
        *nativeW = seq.width;
        *nativeH = seq.height;
        return generator(c, clip, local, std::max(1, int(seq.width * clipScale)), std::max(1, int(seq.height * clipScale)));
    }
}

// ---------------------------------------------------------------------------
// Motion

static void motionMatrix(float m[9], double cw, double ch, double sw, double sh, QPointF pos, double sx, double sy, double rotDeg, QPointF anchor)
{
    const double r = rotDeg * M_PI / 180.0;
    const double c = std::cos(r), s = std::sin(r);
    const double ax = anchor.x(), ay = anchor.y();
    // ndc = 2 * (P*S + R*Sc*(q - A*C)) / S - 1, q = p*C
    m[0] = float(2.0 / sw * c * sx * cw);
    m[1] = float(2.0 / sh * s * sx * cw);
    m[2] = 0;
    m[3] = float(2.0 / sw * -s * sy * ch);
    m[4] = float(2.0 / sh * c * sy * ch);
    m[5] = 0;
    m[6] = float(2.0 / sw * (-c * sx * cw * ax + s * sy * ch * ay) + 2 * pos.x() - 1);
    m[7] = float(2.0 / sh * (-s * sx * cw * ax - c * sy * ch * ay) + 2 * pos.y() - 1);
    m[8] = 1;
}

SurfaceRef Renderer::transform(Ctx &c, const Clip &clip, Ticks local, const SurfaceRef &src, int nativeW, int nativeH, float opacity)
{
    const Sequence &seq = *c.seq;
    SurfaceRef out = m_gl->surface(c.outW, c.outH);
    m_gl->target(out);
    m_gl->clear(0, 0, 0, 0);
    const EffectInstance *mo = clip.effect("motion");
    QPointF pos(0.5, 0.5), anchor(0.5, 0.5);
    double scale = 100, scaleW = 100, rot = 0, flicker = 0;
    bool uniform = true;
    int fit = 0;
    if (mo && mo->enabled) {
        pos = mo->value("position", local).pt();
        anchor = mo->value("anchor", local).pt();
        scale = mo->f("scale", local);
        scaleW = mo->f("scaleWidth", local);
        uniform = mo->value("uniform", local).b();
        rot = mo->f("rotation", local);
        flicker = mo->f("antiFlicker", local);
        fit = mo->value("fit", local).i();
    }
    const double sw = seq.width, sh = seq.height;
    double fx = 1, fy = 1;
    if (clip.source == ClipSource::Media || clip.source == ClipSource::Sequence) {
        if (fit == 0) fx = fy = std::min(sw / nativeW, sh / nativeH);
        else if (fit == 1) fx = fy = std::max(sw / nativeW, sh / nativeH);
        else if (fit == 2) { fx = sw / nativeW; fy = sh / nativeH; }
    } else {
        fx = sw / nativeW;
        fy = sh / nativeH;
    }
    const double sy = fy * scale / 100.0;
    const double sx = fx * (uniform ? scale : scaleW) / 100.0;
    float mat[9];
    motionMatrix(mat, nativeW, nativeH, sw, sh, pos, sx, sy, rot, anchor);
    m_gl->use(m_gl->program("layer", shaders::layer));
    m_gl->bindTexture("uTex", 0, src->tex);
    m_gl->set("uOpacity", opacity);
    m_gl->set("uTexel", 1.f / src->w, 1.f / src->h);
    m_gl->set("uAntiFlicker", float(flicker));
    m_gl->drawQuad(mat);
    return out;
}

// ---------------------------------------------------------------------------

SurfaceRef Renderer::renderClipLayer(Ctx &c, const Track &track, int trackIndex, const Clip &clip, Ticks t, const SurfaceRef &below,
                                     BlendMode *blendOut, float *opacityOut)
{
    const Ticks local = t - clip.start;
    const EffectInstance *op = clip.effect("opacity");
    float opacity = 1.0f;
    BlendMode mode = BlendMode::Normal;
    if (op && op->enabled) {
        opacity = float(std::clamp(op->f("opacity", local) / 100.0, 0.0, 1.0));
        mode = BlendMode(std::clamp(op->value("blend", local).i(), 0, int(BlendMode::Count) - 1));
    }
    *blendOut = mode;
    *opacityOut = opacity;
    if (opacity <= 0.0f)
        return {};

    // Pick the clip-space render scale: no more detail than will be visible.
    double clipScale = c.renderScale;
    if (clip.source == ClipSource::Media) {
        const MediaItem *m = c.project->mediaItem(clip.mediaId);
        const EffectInstance *mo = clip.effect("motion");
        if (m && m->info.width > 0) {
            double userScale = mo ? std::max(mo->f("scale", local), mo->f("scaleWidth", local)) / 100.0 : 1.0;
            double fitScale = std::min(double(c.seq->width) / std::max(1, m->info.width), double(c.seq->height) / std::max(1, m->info.height));
            if (mo && mo->value("fit", local).i() == 1)
                fitScale = std::max(double(c.seq->width) / std::max(1, m->info.width), double(c.seq->height) / std::max(1, m->info.height));
            clipScale = std::clamp(c.renderScale * fitScale * userScale * 1.05, 1.0 / 32.0, 1.0);
            if (c.opt.exporting)
                clipScale = std::clamp(fitScale * userScale * c.renderScale, 1.0 / 8.0, 1.0);
        }
    }
    int nativeW = c.seq->width, nativeH = c.seq->height;
    SurfaceRef src = clipSource(c, clip, t, clipScale, &nativeW, &nativeH, below);
    if (!src)
        return {};
    if (clip.source == ClipSource::Adjustment) {
        // Effects apply to the composite below; Motion is ignored, opacity mixes.
        if (!below)
            return {};
        SurfaceRef copy = m_gl->surface(below->w, below->h);
        m_gl->blit(below, copy);
        return applyEffects(c, clip, local, copy, 1.0, trackIndex, t);
    }
    src = applyEffects(c, clip, local, src, clipScale, trackIndex, t);
    SurfaceRef layer = transform(c, clip, local, src, nativeW, nativeH, 1.0f);
    // Opacity masks.
    if (op && op->enabled && !op->masks.empty()) {
        SurfaceRef empty = m_gl->surface(layer->w, layer->h);
        m_gl->target(empty);
        m_gl->clear(0, 0, 0, 0);
        layer = applyMask(*op, local, empty, layer);
    }
    // Track matte (sequence space).
    for (const auto &e : clip.effects) {
        if (e.type != "trackMatte" || !e.enabled)
            continue;
        int mt = e.value("track", local).i() - 1;
        if (mt == trackIndex || mt < 0)
            continue;
        SurfaceRef matte = trackMatte(c, mt, t);
        SurfaceRef out = m_gl->surface(layer->w, layer->h);
        m_gl->use(m_gl->program("trackMatte", shaders::trackMatte));
        m_gl->target(out);
        m_gl->bindTexture("uLayer", 0, layer->tex);
        if (matte) {
            m_gl->bindTexture("uMatte", 1, matte->tex);
        } else {
            SurfaceRef blank = m_gl->surface(layer->w, layer->h);
            m_gl->target(blank);
            m_gl->clear(0, 0, 0, 0);
            m_gl->use(m_gl->program("trackMatte", shaders::trackMatte));
            m_gl->target(out);
            m_gl->bindTexture("uLayer", 0, layer->tex);
            m_gl->bindTexture("uMatte", 1, blank->tex);
        }
        m_gl->set("uMode", e.value("composite", local).i());
        m_gl->set("uReverse", e.value("reverse", local).b() ? 1 : 0);
        m_gl->drawQuad();
        layer = out;
    }
    (void)track;
    return layer;
}

SurfaceRef Renderer::trackMatte(Ctx &c, int matteTrack, Ticks t)
{
    const Track *mt = c.seq->track(TrackType::Video, matteTrack);
    if (!mt)
        return {};
    int idx = mt->clipIndexAt(t);
    if (idx < 0)
        return {};
    const Clip &clip = mt->clips[idx];
    if (!clip.enabled)
        return {};
    BlendMode bm;
    float op;
    // Avoid infinite recursion: matte clips do not evaluate their own track mattes.
    Clip copy = clip;
    copy.effects.erase(std::remove_if(copy.effects.begin(), copy.effects.end(), [](const EffectInstance &e) { return e.type == "trackMatte"; }),
                       copy.effects.end());
    SurfaceRef l = renderClipLayer(c, *mt, matteTrack, copy, t, {}, &bm, &op);
    if (!l || op >= 0.999f)
        return l;
    SurfaceRef out = m_gl->surface(l->w, l->h);
    m_gl->use(m_gl->program("layer", shaders::layer));
    m_gl->target(out);
    m_gl->bindTexture("uTex", 0, l->tex);
    m_gl->set("uOpacity", op);
    m_gl->set("uAntiFlicker", 0.f);
    m_gl->drawQuad();
    return out;
}

SurfaceRef Renderer::composite(Ctx &c, Ticks t)
{
    const Sequence &seq = *c.seq;
    SurfaceRef acc = m_gl->surface(c.outW, c.outH);
    m_gl->target(acc);
    m_gl->clear(0, 0, 0, 0);
    for (int ti = 0; ti < (int)seq.videoTracks.size(); ++ti) {
        const Track &track = seq.videoTracks[ti];
        if (track.muted)
            continue;
        // Transition active at t?
        const Transition *active = nullptr;
        TimeRange tr;
        for (const auto &x : track.transitions) {
            TimeRange r = edit::transitionRange(track, x);
            if (r.contains(t)) {
                active = &x;
                tr = r;
                break;
            }
        }
        if (active) {
            const Clip *a = active->clipA.isEmpty() ? nullptr : track.clip(active->clipA);
            const Clip *b = active->clipB.isEmpty() ? nullptr : track.clip(active->clipB);
            BlendMode bm = BlendMode::Normal;
            float op = 1.f;
            SurfaceRef la, lb;
            if (a && a->enabled) {
                la = renderClipLayer(c, track, ti, *a, t, acc, &bm, &op);
                if (la && op < 0.999f) {
                    SurfaceRef o2 = m_gl->surface(la->w, la->h);
                    m_gl->use(m_gl->program("layer", shaders::layer));
                    m_gl->target(o2);
                    m_gl->bindTexture("uTex", 0, la->tex);
                    m_gl->set("uOpacity", op);
                    m_gl->set("uAntiFlicker", 0.f);
                    m_gl->drawQuad();
                    la = o2;
                }
            }
            if (b && b->enabled) {
                lb = renderClipLayer(c, track, ti, *b, t, acc, &bm, &op);
                if (lb && op < 0.999f) {
                    SurfaceRef o2 = m_gl->surface(lb->w, lb->h);
                    m_gl->use(m_gl->program("layer", shaders::layer));
                    m_gl->target(o2);
                    m_gl->bindTexture("uTex", 0, lb->tex);
                    m_gl->set("uOpacity", op);
                    m_gl->set("uAntiFlicker", 0.f);
                    m_gl->drawQuad();
                    lb = o2;
                }
            }
            auto blank = [&] {
                SurfaceRef s = m_gl->surface(c.outW, c.outH);
                m_gl->target(s);
                m_gl->clear(0, 0, 0, 0);
                return s;
            };
            if (!la) la = blank();
            if (!lb) lb = blank();
            double p = tr.duration() > 0 ? double(t - tr.start) / double(tr.duration()) : 1.0;
            SurfaceRef mixed = transition(c, *active, p, la, lb);
            acc = blend(acc, mixed, BlendMode::Normal, 1.0f);
            continue;
        }
        int idx = track.clipIndexAt(t);
        if (idx < 0)
            continue;
        const Clip &clip = track.clips[idx];
        if (!clip.enabled)
            continue;
        BlendMode bm;
        float op;
        SurfaceRef layer = renderClipLayer(c, track, ti, clip, t, acc, &bm, &op);
        if (!layer)
            continue;
        if (clip.source == ClipSource::Adjustment) {
            // mix(below, fx(below), opacity) == normal blend of an opaque layer with opacity.
            acc = blend(acc, layer, BlendMode::Normal, op);
        } else {
            acc = blend(acc, layer, bm, op);
        }
    }
    if (c.opt.showCaptions && c.depth == 0)
        acc = captions(c, t, acc);
    return acc;
}

SurfaceRef Renderer::captions(Ctx &c, Ticks t, const SurfaceRef &base)
{
    SurfaceRef out = base;
    for (const auto &ct : c.seq->captionTracks) {
        if (!ct.visible || (c.opt.exporting && !ct.burnIn))
            continue;
        for (const auto &cap : ct.captions) {
            if (t < cap.start || t >= cap.end)
                continue;
            int highlight = -1;
            if (ct.style.wordHighlight) {
                const int words = std::max<int>(1, cap.text.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts).size());
                highlight = int(double(t - cap.start) / double(std::max<Ticks>(1, cap.end - cap.start)) * words);
            }
            QString key = QString("cap_%1_%2x%3_%4_%5").arg(qHash(cap.text)).arg(c.outW).arg(c.outH).arg(highlight).arg(qHash(QJsonDocument(ct.style.toJson()).toJson()));
            GLuint tex = cachedImageTexture(key, [&] { return TitleRenderer::renderCaption(ct.style, cap.text, c.outW, c.outH, highlight); });
            SurfaceRef layer = m_gl->surface(c.outW, c.outH);
            m_gl->use(m_gl->program("blit_img", R"(#version 330 core
in vec2 vUv; out vec4 o; uniform sampler2D uTex; void main(){ o = texture(uTex, vUv); })"));
            m_gl->target(layer);
            m_gl->bindTexture("uTex", 0, tex);
            m_gl->drawQuad();
            out = blend(out, layer, BlendMode::Normal, 1.0f);
            break;
        }
    }
    return out;
}

SurfaceRef Renderer::renderSequence(const Project &project, const Sequence &seq, Ticks t, const RenderOptions &opt)
{
    if (!m_ok)
        return {};
    m_misses = 0;
    Ctx c;
    c.project = &project;
    c.seq = &seq;
    c.opt = opt;
    c.outW = std::max(1, opt.width);
    c.outH = std::max(1, opt.height);
    c.renderScale = double(c.outW) / std::max(1, seq.width);
    SurfaceRef acc = composite(c, t);
    if (!opt.keepAlpha) {
        // Composite over opaque black.
        SurfaceRef black = m_gl->surface(c.outW, c.outH);
        m_gl->target(black);
        m_gl->clear(0, 0, 0, 1);
        acc = blend(black, acc, BlendMode::Normal, 1.0f);
    }
    m_gl->trimPool();
    return acc;
}

SurfaceRef Renderer::renderMedia(const Project &project, const MediaItem &item, Ticks t, int w, int h, bool useProxy)
{
    if (!m_ok)
        return {};
    QString path = (useProxy && project.settings.useProxies && !item.proxyPath.isEmpty()) ? item.proxyPath : item.path;
    VideoFramePtr frame = item.info.isStill ? MediaPool::instance().image(path) : MediaPool::instance().videoFrame(path, t, item.fps());
    SurfaceRef out = m_gl->surface(w, h);
    m_gl->target(out);
    m_gl->clear(0, 0, 0, 1);
    if (!frame)
        return out;
    const bool rot90 = item.info.rotation == 90 || item.info.rotation == 270;
    double nw = (rot90 ? item.info.height : item.info.width) * item.info.pixelAspect;
    double nh = rot90 ? item.info.width : item.info.height;
    double s = std::min(w / std::max(1.0, nw), h / std::max(1.0, nh));
    const int decodedH = (frame->rotation == 90 || frame->rotation == 270) ? frame->width : frame->height;
    SurfaceRef src = convertFrame(frame, std::min(1.0, s * 1.05 * nh / std::max(1, decodedH)), item.alphaMode);
    float mat[9];
    motionMatrix(mat, nw, nh, w, h, {0.5, 0.5}, s, s, 0, {0.5, 0.5});
    m_gl->use(m_gl->program("layer", shaders::layer));
    m_gl->target(out);
    m_gl->bindTexture("uTex", 0, src->tex);
    m_gl->set("uOpacity", 1.0f);
    m_gl->set("uAntiFlicker", 0.f);
    m_gl->f->glEnable(GL_BLEND);
    m_gl->f->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    m_gl->drawQuad(mat);
    m_gl->f->glDisable(GL_BLEND);
    return out;
}

void Renderer::present(const SurfaceRef &s, int fbW, int fbH, QRectF vp, bool checkerboard, float zoom, QPointF pan)
{
    auto *f = m_gl->f;
    f->glViewport(int(vp.x()), int(fbH - vp.y() - vp.height()), int(vp.width()), int(vp.height()));
    m_gl->use(m_gl->program("present", shaders::present));
    if (s)
        m_gl->bindTexture("uTex", 0, s->tex);
    m_gl->set("uChecker", checkerboard ? 1 : 0);
    m_gl->set("uSize", float(vp.width()), float(vp.height()));
    const float w = 1.0f / std::max(0.01f, zoom);
    m_gl->set("uUvRect", float(0.5 - w / 2 + pan.x()), float(0.5 - w / 2 + pan.y()), w, w);
    m_gl->drawQuad();
    (void)fbW;
}

} // namespace rc
