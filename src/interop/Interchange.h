#pragma once
// Project interchange with other editors.
//  - Final Cut Pro 7 XML (xmeml): the format Premiere Pro imports/exports via
//    File > Export > Final Cut Pro XML. Full round trip of cuts, tracks,
//    speed, opacity/volume levels, markers and cross dissolves.
//  - CMX 3600 EDL (one video track + audio), import and export.
//  - OpenTimelineIO (.otio JSON), import and export.
//  - FCPXML 1.9 export (Final Cut Pro X, DaVinci Resolve).
//  - Adobe Premiere Pro project (.prproj) import: media, bins and sequences
//    (clips with in/out and track placement).
#include "core/Model.h"

namespace rc::interop {

// Export functions write one sequence; nested sequences are written as nested
// sequences where the format supports it.
bool exportFcp7Xml(const Project &project, const Sequence &seq, const QString &path, QString *error = nullptr);
bool exportEdl(const Project &project, const Sequence &seq, const QString &path, QString *error = nullptr);
bool exportOtio(const Project &project, const Sequence &seq, const QString &path, QString *error = nullptr);
bool exportFcpxml(const Project &project, const Sequence &seq, const QString &path, QString *error = nullptr);

// Import functions add media items and sequences to `project` (media files are
// probed). Returns the ids of the created sequences.
QStringList importFcp7Xml(const QString &path, Project &project, QString *error = nullptr);
QStringList importEdl(const QString &path, Project &project, FrameRate fps, QString *error = nullptr);
QStringList importOtio(const QString &path, Project &project, QString *error = nullptr);
QStringList importPrproj(const QString &path, Project &project, QString *error = nullptr);

// Dispatch by extension (.xml, .edl, .otio, .prproj, .fcpxml export only).
QStringList importAny(const QString &path, Project &project, FrameRate fpsHint, QString *error = nullptr);
bool exportAny(const Project &project, const Sequence &seq, const QString &path, QString *error = nullptr);

} // namespace rc::interop
