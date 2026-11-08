#pragma once
// OpenFX host: loads image effect plugins (.ofx.bundle) and exposes them as
// regular video effects ("ofx.<plugin identifier>"). Rendering is on the CPU
// in 32-bit float premultiplied RGBA, bottom-up rows (OFX convention).
#include "core/Effects.h"
#include <QStringList>
#include <memory>

namespace rc::ofx {

struct RenderArgs {
    const float *src = nullptr;   // w * h * 4 floats, top-down rows
    float *dst = nullptr;         // same layout
    int width = 0, height = 0;
    double renderScale = 1.0;     // output pixels per canonical (sequence) pixel
    double time = 0;              // frames, relative to the clip start
    double fps = 25;
    double duration = 0;          // frames
};

class Host {
public:
    static Host &instance();
    ~Host();
    // Directories searched: $OFX_PLUGIN_PATH, /usr/OFX/Plugins,
    // /usr/local/OFX/Plugins, ~/.local/share/raccord/ofx, plus `extra`.
    QStringList searchPaths() const;
    // Loads every plugin found and registers its effect definition. Safe to call once.
    int loadPlugins(const QStringList &extra = {});
    QStringList loadedPlugins() const;
    QStringList errors() const;
    static bool isOfxEffect(const QString &effectId) { return effectId.startsWith("ofx."); }
    // Renders effect `e` (parameters evaluated at `local`). `instanceKey`
    // identifies the plugin instance (one per effect uid and render thread).
    bool render(const EffectInstance &e, Ticks local, const QString &instanceKey, const RenderArgs &args, QString *error = nullptr);

private:
    Host();
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace rc::ofx
