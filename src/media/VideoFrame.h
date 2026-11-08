#pragma once
// A decoded picture, kept in its native planar layout so that colour
// conversion happens on the GPU.
#include "core/Time.h"
#include <memory>
#include <cstdint>
#include <QImage>

struct AVFrame;

namespace rc {

enum class PixelLayout {
    Invalid,
    Planar,     // Y, U, V (+A) planes, chroma subsampled by chromaShiftX/Y
    SemiPlanar, // NV12 / P010: Y plane + interleaved UV
    Rgba,       // packed RGBA, 8 or 16 bit per component
    Gray,
};

enum class YuvMatrix { Bt601, Bt709, Bt2020, Identity };
enum class TransferFn { Bt709, Srgb, Linear, Pq, Hlg };

struct VideoFrame {
    int width = 0;
    int height = 0;
    PixelLayout layout = PixelLayout::Invalid;
    int bitDepth = 8;           // significant bits per component
    bool msbAligned = false;    // P010/P016 style: value stored in the high bits
    int chromaShiftX = 0;       // 1 for 4:2:x
    int chromaShiftY = 0;       // 1 for 4:2:0
    bool hasAlpha = false;
    bool fullRange = false;
    bool bgr = false;           // packed BGRA order
    YuvMatrix matrix = YuvMatrix::Bt709;
    TransferFn transfer = TransferFn::Bt709;
    double pixelAspect = 1.0;
    int rotation = 0;
    Ticks pts = 0;              // media time (ticks, relative to media start)
    Ticks duration = 0;

    // Plane data. When `av` is set, planes point into it.
    const uint8_t *data[4] = {nullptr, nullptr, nullptr, nullptr};
    int linesize[4] = {0, 0, 0, 0};
    std::shared_ptr<AVFrame> av;        // owning reference to the FFmpeg frame
    std::shared_ptr<std::vector<uint8_t>> owned;  // or owned buffer (images)

    int planeCount() const;
    int bytesPerComponent() const { return bitDepth > 8 ? 2 : 1; }
    size_t byteSize() const;
    bool valid() const { return layout != PixelLayout::Invalid && width > 0 && height > 0; }

    // Wraps a QImage (converted to RGBA8888 premultiplied=false).
    static std::shared_ptr<VideoFrame> fromImage(const QImage &img);
    // CPU conversion for thumbnails/tests (slow path).
    QImage toImage(int maxWidth = 0) const;
};

using VideoFramePtr = std::shared_ptr<const VideoFrame>;

} // namespace rc
