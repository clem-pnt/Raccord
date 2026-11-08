#pragma once
#include "ExportSettings.h"
#include "core/Model.h"
#include <QImage>
#include <functional>

class QOpenGLContext;
class QOffscreenSurface;

namespace rc {

// Offscreen OpenGL context usable from any thread (surface must be created on the GUI thread).
struct OffscreenGL {
    QOpenGLContext *context = nullptr;
    QOffscreenSurface *surface = nullptr;
    // Creates the surface (call on the GUI thread).
    static QOffscreenSurface *createSurface();
    // Creates and makes current a context in the calling thread.
    bool create(QOffscreenSurface *surface, QString *error = nullptr);
    void destroy();
};

struct ExportProgress {
    int64_t frame = 0;
    int64_t totalFrames = 0;
    double fraction = 0;
    double fps = 0;
    double etaSeconds = 0;
    QImage preview;   // occasionally set
};

using ExportCallback = std::function<bool(const ExportProgress &)>;   // return false to cancel

class Exporter {
public:
    // Renders a sequence to a file. Must run in a thread where `surface` can be used.
    static bool run(const Project &project, const QString &sequenceId, const ExportSettings &settings, QOffscreenSurface *surface,
                    const ExportCallback &cb, QString *error);
    // Measures integrated loudness (LUFS) of the mixed sequence audio over a range.
    static double measureLoudness(const Project &project, const Sequence &seq, TimeRange range, double *truePeakDb = nullptr);
};

} // namespace rc
