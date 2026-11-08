#pragma once
#include "core/Time.h"
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <vector>

namespace rc {

struct ExportSettings {
    QString path;
    QString container = "mp4";      // mp4, mov, mkv, webm, mxf, png, tiff, jpg (image sequence), wav, mp3, flac, m4a
    QString videoCodec = "h264";    // h264, hevc, prores, dnxhr, vp9, av1, h264_nvenc, hevc_nvenc, png, tiff, mjpeg, none
    int width = 0;                  // 0 = sequence size
    int height = 0;
    FrameRate fps{0, 1};            // 0 = sequence rate
    QString rateControl = "crf";    // crf, vbr, cbr
    int quality = 20;               // CRF / QP
    int videoBitrateKbps = 16000;
    QString encoderPreset = "medium";
    QString profile;                // prores: proxy/lt/standard/hq/4444/4444xq; dnxhr: lb/sq/hq/hqx/444
    bool tenBit = false;
    bool alpha = false;
    QString audioCodec = "aac";     // aac, opus, flac, pcm16, pcm24, mp3, none
    int audioBitrateKbps = 320;
    int sampleRate = 0;             // 0 = sequence rate
    int channels = 2;
    bool useWorkArea = true;        // in/out or full sequence
    Ticks rangeStart = kInvalidTicks;
    Ticks rangeEnd = kInvalidTicks;
    bool useProxies = false;
    double loudnessTarget = 0;      // LUFS, 0 = off (e.g. -14 for YouTube/Spotify)
    double truePeakDb = -1.0;
    bool burnCaptions = false;
    bool sidecarSrt = false;
    bool fastStart = true;
    int keyframeInterval = 0;       // frames, 0 = default (2 s)
    QString title;                  // metadata

    QJsonObject toJson() const;
    static ExportSettings fromJson(const QJsonObject &o);
    QString extension() const;
    bool isImageSequence() const { return container == "png" || container == "tiff" || container == "jpg"; }
    bool isAudioOnly() const { return videoCodec == "none" || container == "wav" || container == "mp3" || container == "flac" || container == "m4a"; }
};

struct ExportPreset {
    QString id;
    QString name;
    QString category;
    QString description;
    ExportSettings settings;
};

const std::vector<ExportPreset> &exportPresets();
const ExportPreset *findPreset(const QString &id);
// Available encoders on this machine (checks FFmpeg at runtime).
bool encoderAvailable(const QString &videoCodec);

} // namespace rc
