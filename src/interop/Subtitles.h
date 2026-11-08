#pragma once
#include "core/Model.h"

namespace rc {

// SubRip / WebVTT import and export. `offset` is subtracted from caption times on export.
bool writeSrt(const CaptionTrack &track, const QString &path, Ticks offset = 0);
bool writeVtt(const CaptionTrack &track, const QString &path, Ticks offset = 0);
QString toSrt(const CaptionTrack &track, Ticks offset = 0);
bool readSubtitles(const QString &path, CaptionTrack &out, QString *error = nullptr);   // .srt or .vtt
CaptionTrack parseSrt(const QString &text);
// Splits long captions to respect style.maxCharsPerLine * maxLines.
void reflowCaptions(CaptionTrack &track);

} // namespace rc
