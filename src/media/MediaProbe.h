#pragma once
#include "core/Model.h"

namespace rc {

// Reads technical information about a media file (fast: header + stream info).
MediaInfo probeMedia(const QString &path);
// Detects image sequences: "shot_0001.png" -> "shot_%04d.png" if neighbours exist.
QString detectImageSequence(const QString &path, int *firstNumber = nullptr, int *count = nullptr);
// File extensions Raccord can import (for file dialogs).
QStringList importFilters();
bool isImportable(const QString &path);

} // namespace rc
