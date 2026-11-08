#include "VideoFrame.h"
#include "FFmpeg.h"
#include <cstring>

namespace rc {

int VideoFrame::planeCount() const
{
    switch (layout) {
    case PixelLayout::Planar: return hasAlpha ? 4 : 3;
    case PixelLayout::SemiPlanar: return 2;
    case PixelLayout::Rgba:
    case PixelLayout::Gray: return 1;
    default: return 0;
    }
}

size_t VideoFrame::byteSize() const
{
    size_t s = 0;
    for (int p = 0; p < planeCount(); ++p) {
        int h = height;
        if (p > 0 && p < 3 && layout != PixelLayout::Rgba)
            h = (height + (1 << chromaShiftY) - 1) >> chromaShiftY;
        s += size_t(std::abs(linesize[p])) * h;
    }
    return s + sizeof(VideoFrame);
}

std::shared_ptr<VideoFrame> VideoFrame::fromImage(const QImage &src)
{
    QImage img = src.convertToFormat(QImage::Format_RGBA8888);
    auto f = std::make_shared<VideoFrame>();
    f->width = img.width();
    f->height = img.height();
    f->layout = PixelLayout::Rgba;
    f->bitDepth = 8;
    f->hasAlpha = img.hasAlphaChannel();
    f->fullRange = true;
    f->matrix = YuvMatrix::Identity;
    f->transfer = TransferFn::Srgb;
    f->owned = std::make_shared<std::vector<uint8_t>>(size_t(img.bytesPerLine()) * img.height());
    std::memcpy(f->owned->data(), img.constBits(), f->owned->size());
    f->data[0] = f->owned->data();
    f->linesize[0] = img.bytesPerLine();
    return f;
}

static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

QImage VideoFrame::toImage(int maxWidth) const
{
    if (!valid())
        return {};
    int ow = width, oh = height;
    if (maxWidth > 0 && ow > maxWidth) {
        oh = std::max(1, int(int64_t(oh) * maxWidth / ow));
        ow = maxWidth;
    }
    QImage out(ow, oh, QImage::Format_RGBA8888);
    const float maxV = float((1 << bitDepth) - 1);
    auto sample = [&](int plane, int x, int y) -> float {
        const uint8_t *row = data[plane] + int64_t(y) * linesize[plane];
        if (bytesPerComponent() == 1)
            return row[x] / 255.0f;
        uint16_t v = reinterpret_cast<const uint16_t *>(row)[x];
        return msbAligned ? v / 65535.0f : v / maxV;
    };
    float kr = 0.2126f, kb = 0.0722f;
    if (matrix == YuvMatrix::Bt601) { kr = 0.299f; kb = 0.114f; }
    if (matrix == YuvMatrix::Bt2020) { kr = 0.2627f; kb = 0.0593f; }
    const float kg = 1 - kr - kb;
    for (int oy = 0; oy < oh; ++oy) {
        uchar *dst = out.scanLine(oy);
        int y = int(int64_t(oy) * height / oh);
        for (int ox = 0; ox < ow; ++ox) {
            int x = int(int64_t(ox) * width / ow);
            float r = 0, g = 0, b = 0, a = 1;
            if (layout == PixelLayout::Rgba) {
                const uint8_t *row = data[0] + int64_t(y) * linesize[0];
                if (bitDepth > 8) {
                    const uint16_t *p = reinterpret_cast<const uint16_t *>(row) + x * 4;
                    r = p[0] / 65535.f; g = p[1] / 65535.f; b = p[2] / 65535.f; a = p[3] / 65535.f;
                } else {
                    const uint8_t *p = row + x * 4;
                    r = p[0] / 255.f; g = p[1] / 255.f; b = p[2] / 255.f; a = p[3] / 255.f;
                }
                if (bgr) std::swap(r, b);
            } else if (layout == PixelLayout::Gray) {
                r = g = b = sample(0, x, y);
            } else {
                float Y = sample(0, x, y);
                float U, V;
                int cx = x >> chromaShiftX, cy = y >> chromaShiftY;
                if (layout == PixelLayout::SemiPlanar) {
                    const uint8_t *row = data[1] + int64_t(cy) * linesize[1];
                    if (bytesPerComponent() == 1) { U = row[cx * 2] / 255.f; V = row[cx * 2 + 1] / 255.f; }
                    else {
                        const uint16_t *p = reinterpret_cast<const uint16_t *>(row);
                        U = p[cx * 2] / 65535.f; V = p[cx * 2 + 1] / 65535.f;
                    }
                } else {
                    U = sample(1, cx, cy);
                    V = sample(2, cx, cy);
                }
                if (!fullRange) {
                    Y = (Y - 16.f / 255.f) * (255.f / 219.f);
                    U = (U - 128.f / 255.f) * (255.f / 224.f);
                    V = (V - 128.f / 255.f) * (255.f / 224.f);
                } else {
                    U -= 0.5f; V -= 0.5f;
                }
                r = Y + 2 * (1 - kr) * V;
                b = Y + 2 * (1 - kb) * U;
                g = (Y - kr * r - kb * b) / kg;
                if (hasAlpha && layout == PixelLayout::Planar)
                    a = sample(3, x, y);
            }
            dst[ox * 4 + 0] = uchar(clamp01(r) * 255 + 0.5f);
            dst[ox * 4 + 1] = uchar(clamp01(g) * 255 + 0.5f);
            dst[ox * 4 + 2] = uchar(clamp01(b) * 255 + 0.5f);
            dst[ox * 4 + 3] = uchar(clamp01(a) * 255 + 0.5f);
        }
    }
    return out;
}

} // namespace rc
