#include "FFmpeg.h"
#include <QFileInfo>
#include <QMutex>
#include <QRegularExpression>
#include <map>
#include <cstdlib>

namespace rc::ff {

QString errorString(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof buf);
    return QString::fromUtf8(buf);
}

void initLogging()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    const char *lvl = std::getenv("RACCORD_FFMPEG_LOG");
    av_log_set_level(lvl ? std::atoi(lvl) : AV_LOG_ERROR);
}

FormatCtx openInput(const QString &path, QString *error)
{
    initLogging();
    AVFormatContext *ctx = nullptr;
    AVDictionary *opts = nullptr;
    const AVInputFormat *fmt = nullptr;
    QByteArray p = path.toUtf8();
    if (path.contains(QRegularExpression("%0?\\d*d"))) {
        fmt = av_find_input_format("image2");
    }
    av_dict_set(&opts, "probesize", "20000000", 0);
    int r = avformat_open_input(&ctx, p.constData(), fmt, &opts);
    av_dict_free(&opts);
    if (r < 0) {
        if (error) *error = errorString(r);
        return {};
    }
    FormatCtx f(ctx);
    r = avformat_find_stream_info(ctx, nullptr);
    if (r < 0) {
        if (error) *error = errorString(r);
        return {};
    }
    return f;
}

AVHWDeviceType preferredHwType()
{
    const char *env = std::getenv("RACCORD_HWACCEL");
    QString s = env ? QString(env).toLower() : QString("auto");
    if (s == "none" || s == "off" || s == "0")
        return AV_HWDEVICE_TYPE_NONE;
    if (s == "cuda" || s == "nvdec")
        return AV_HWDEVICE_TYPE_CUDA;
    if (s == "vaapi")
        return AV_HWDEVICE_TYPE_VAAPI;
    if (s == "vulkan")
        return AV_HWDEVICE_TYPE_VULKAN;
    // auto: CUDA if an NVIDIA device exists, else VAAPI if a render node exists.
    if (QFileInfo::exists("/dev/nvidia0"))
        return AV_HWDEVICE_TYPE_CUDA;
    if (QFileInfo::exists("/dev/dri/renderD128"))
        return AV_HWDEVICE_TYPE_VAAPI;
    return AV_HWDEVICE_TYPE_NONE;
}

AVBufferRef *hwDevice(AVHWDeviceType type)
{
    static QMutex mutex;
    static std::map<int, AVBufferRef *> devices;
    static std::map<int, bool> failed;
    if (type == AV_HWDEVICE_TYPE_NONE)
        return nullptr;
    QMutexLocker lock(&mutex);
    if (failed[type])
        return nullptr;
    auto it = devices.find(type);
    if (it != devices.end())
        return it->second;
    AVBufferRef *dev = nullptr;
    if (av_hwdevice_ctx_create(&dev, type, nullptr, nullptr, 0) < 0) {
        failed[type] = true;
        return nullptr;
    }
    devices[type] = dev;
    return dev;
}

} // namespace rc::ff
