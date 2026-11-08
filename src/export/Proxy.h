#pragma once
// Proxy generation: low resolution, edit friendly H.264 copies of the video.
#include "core/Model.h"
#include <functional>

namespace rc {

// Writes a video-only proxy of `src` with the given height (width follows the
// aspect ratio). Keyframe every 6 frames for fast scrubbing.
bool makeProxy(const MediaItem &src, const QString &dst, int height, const std::function<bool(double)> &progress, QString *error);
// Path where the proxy of a media item is stored in the project cache.
QString proxyPathFor(const QString &cacheDir, const MediaItem &m, int height);

} // namespace rc
