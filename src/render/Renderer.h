#pragma once
// GPU compositor: turns a Sequence at a given time into an image.
#include "GL.h"
#include "core/Model.h"
#include "media/VideoFrame.h"
#include <QHash>
#include <map>
#include <memory>

namespace rc {

struct RenderOptions {
    int width = 1920;              // output size (may be smaller than the sequence for previews)
    int height = 1080;
    bool useProxies = true;
    bool showCaptions = true;      // captions overlay (burn-in tracks always render when exporting)
    bool exporting = false;
    bool keepAlpha = false;        // otherwise composited over black
    bool showHslMask = true;       // honour "show mask" toggles (disabled when exporting)
    QString hideTrackMatteSources; // reserved
    int maxNesting = 8;
    int soloVideoTrack = -1;       // render only this video track of the top sequence (multicam angle preview)
};

class TitleRenderer;

class Renderer {
public:
    Renderer();          // needs a current OpenGL context
    ~Renderer();
    bool ok() const { return m_ok; }
    QString error() const { return m_error; }
    GL &gl() { return *m_gl; }

    // Full composite of a sequence at sequence time t.
    SurfaceRef renderSequence(const Project &project, const Sequence &seq, Ticks t, const RenderOptions &opt);
    // A single media item at media time t, fit in w x h (source monitor).
    SurfaceRef renderMedia(const Project &project, const MediaItem &item, Ticks t, int w, int h, bool useProxy = true);
    // Uploads + converts a decoded frame to an upright, premultiplied RGBA16F surface.
    SurfaceRef convertFrame(const VideoFramePtr &frame, double scale = 1.0, int alphaMode = 0);
    // Draws a surface into the currently bound framebuffer (screen) letterboxed in `vp` (pixels, GL origin bottom-left).
    void present(const SurfaceRef &s, int fbW, int fbH, QRectF vp, bool checkerboard, float zoom = 1.0f, QPointF pan = {});
    // Blends a premultiplied overlay image (UI drawn with QPainter) over the whole bound framebuffer.
    void presentOverlay(const QImage &img, int fbW, int fbH);
    // Downscaled copy for scopes.
    SurfaceRef downscale(const SurfaceRef &s, int maxW);

    void releaseCaches();
    // Statistics for the info overlay.
    int lastDecodeMisses() const { return m_misses; }

private:
    struct Ctx {
        const Project *project = nullptr;
        const Sequence *seq = nullptr;
        RenderOptions opt;
        int outW = 0, outH = 0;
        double renderScale = 1.0;   // output px per sequence px
        int depth = 0;
        int soloTrack = -1;
    };
    SurfaceRef composite(Ctx &c, Ticks t);
    // Renders one clip at sequence time t into a full output-size layer (after effects + motion). opacity applied.
    SurfaceRef renderClipLayer(Ctx &c, const Track &track, int trackIndex, const Clip &clip, Ticks t, const SurfaceRef &below,
                               BlendMode *blendOut, float *opacityOut);
    SurfaceRef clipSource(Ctx &c, const Clip &clip, Ticks t, double clipScale, int *nativeW, int *nativeH, const SurfaceRef &below);
    SurfaceRef applyEffects(Ctx &c, const Clip &clip, Ticks local, SurfaceRef src, double clipScale, int trackIndex, Ticks seqTime);
    SurfaceRef applyEffect(Ctx &c, const EffectInstance &e, Ticks local, SurfaceRef src, double clipScale, int trackIndex, Ticks seqTime);
    SurfaceRef applyMask(const EffectInstance &e, Ticks local, const SurfaceRef &orig, const SurfaceRef &fx);
    SurfaceRef transform(Ctx &c, const Clip &clip, Ticks local, const SurfaceRef &src, int nativeW, int nativeH, float opacity);
    SurfaceRef transition(Ctx &c, const Transition &tr, double progress, const SurfaceRef &a, const SurfaceRef &b);
    SurfaceRef blend(const SurfaceRef &base, const SurfaceRef &layer, BlendMode mode, float opacity);
    SurfaceRef blur(const SurfaceRef &src, float radiusPx, int dims, bool repeatEdges = true);
    SurfaceRef generator(Ctx &c, const Clip &clip, Ticks local, int w, int h);
    SurfaceRef captions(Ctx &c, Ticks t, const SurfaceRef &base);
    SurfaceRef trackMatte(Ctx &c, int matteTrack, Ticks t);

    GLuint lut3d(const QString &path, int *size);
    GLuint curveTexture(const QString &json, bool hue);

    std::unique_ptr<GL> m_gl;
    std::unique_ptr<TitleRenderer> m_titles;
    bool m_ok = false;
    QString m_error;
    int m_misses = 0;
    GLuint m_overlayTex = 0;

    // Plane textures used to upload frames.
    GLuint m_planes[4] = {0, 0, 0, 0};
    int m_planeW[4] = {0, 0, 0, 0}, m_planeH[4] = {0, 0, 0, 0};
    GLenum m_planeFmt[4] = {0, 0, 0, 0};
    struct Converted { std::weak_ptr<const VideoFrame> frame; SurfaceRef surface; double scale; int alphaMode; };
    std::vector<Converted> m_converted;
    struct LutEntry { GLuint tex = 0; int size = 0; };
    QHash<QString, LutEntry> m_luts;
    QHash<QString, GLuint> m_curves;
    QHash<QString, GLuint> m_imageTex;   // text/caption textures by key
    std::vector<QString> m_imageTexOrder;
    GLuint cachedImageTexture(const QString &key, const std::function<QImage()> &make);
};

} // namespace rc
