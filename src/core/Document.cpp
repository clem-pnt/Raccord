#include "Document.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

namespace rc {

Document::Document(QObject *parent) : QObject(parent)
{
    newProject();
    connect(&m_autosaveTimer, &QTimer::timeout, this, [this] {
        if (m_dirty)
            autosave();
    });
    m_autosaveTimer.start(5 * 60 * 1000);
}

QString Document::projectDir() const
{
    if (m_path.isEmpty())
        return QDir::homePath();
    return QFileInfo(m_path).absolutePath();
}

QString Document::cacheDir() const
{
    QString dir;
    if (!m_project.settings.scratchDir.isEmpty())
        dir = m_project.settings.scratchDir;
    else if (!m_path.isEmpty())
        dir = projectDir() + "/" + QFileInfo(m_path).completeBaseName() + " Cache";
    else
        dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/unsaved";
    QDir().mkpath(dir);
    return dir;
}

QByteArray Document::snapshot() const
{
    return qCompress(QJsonDocument(m_project.toJson()).toJson(QJsonDocument::Compact), 3);
}

void Document::restore(const QByteArray &snap)
{
    QJsonDocument d = QJsonDocument::fromJson(qUncompress(snap));
    m_project = Project::fromJson(d.object());
}

void Document::setDirty(bool d)
{
    if (m_dirty == d)
        return;
    m_dirty = d;
    emit dirtyChanged(d);
}

bool Document::edit(const QString &label, const std::function<bool(Project &)> &fn)
{
    QByteArray before = snapshot();
    Project backup = m_project;
    if (!fn(m_project)) {
        m_project = std::move(backup);
        return false;
    }
    QByteArray after = snapshot();
    if (after == before)
        return true;
    m_undo.push_back({label, before, after});
    while ((int)m_undo.size() > m_maxUndo)
        m_undo.pop_front();
    m_redo.clear();
    m_mergeKey.clear();
    setDirty(true);
    emit historyChanged();
    emit changed();
    return true;
}

bool Document::editSequence(const QString &label, const std::function<bool(Sequence &)> &fn)
{
    return edit(label, [&](Project &p) {
        Sequence *s = p.activeSequence();
        return s && fn(*s);
    });
}

bool Document::editMerged(const QString &label, const QString &mergeKey, const std::function<bool(Project &)> &fn)
{
    if (!mergeKey.isEmpty() && mergeKey == m_mergeKey && !m_undo.empty()) {
        Project backup = m_project;
        if (!fn(m_project)) {
            m_project = std::move(backup);
            return false;
        }
        m_undo.back().after = snapshot();
        m_redo.clear();
        setDirty(true);
        emit changed();
        return true;
    }
    bool ok = edit(label, fn);
    if (ok)
        m_mergeKey = mergeKey;
    return ok;
}

void Document::touch(bool markDirty)
{
    if (markDirty)
        setDirty(true);
    emit changed();
}

QStringList Document::history() const
{
    QStringList l;
    for (const auto &s : m_undo)
        l << s.label;
    return l;
}

void Document::undo()
{
    if (m_undo.empty())
        return;
    Step s = m_undo.back();
    m_undo.pop_back();
    // Keep the playhead where the user is.
    Ticks playhead = m_project.activeSequence() ? m_project.activeSequence()->playhead : 0;
    QString active = m_project.activeSequenceId;
    restore(s.before);
    m_project.activeSequenceId = active;
    if (auto *seq = m_project.activeSequence())
        seq->playhead = playhead;
    m_redo.push_back(s);
    m_mergeKey.clear();
    setDirty(true);
    emit historyChanged();
    emit changed();
}

void Document::redo()
{
    if (m_redo.empty())
        return;
    Step s = m_redo.back();
    m_redo.pop_back();
    Ticks playhead = m_project.activeSequence() ? m_project.activeSequence()->playhead : 0;
    QString active = m_project.activeSequenceId;
    restore(s.after);
    m_project.activeSequenceId = active;
    if (auto *seq = m_project.activeSequence())
        seq->playhead = playhead;
    m_undo.push_back(s);
    m_mergeKey.clear();
    setDirty(true);
    emit historyChanged();
    emit changed();
}

void Document::newProject()
{
    m_project = Project();
    Sequence s;
    s.id = newId();
    s.name = "Sequence 01";
    s.ensureDefaultTracks();
    m_project.activeSequenceId = s.id;
    m_project.sequences.push_back(s);
    m_path.clear();
    m_undo.clear();
    m_redo.clear();
    setDirty(false);
    emit historyChanged();
    emit projectReplaced();
    emit changed();
}

QByteArray Document::serialize(const Project &p, const QString &projectDir)
{
    Project copy = p;
    if (!projectDir.isEmpty())
        copy.makePathsRelative(projectDir);
    return QJsonDocument(copy.toJson()).toJson(QJsonDocument::Indented);
}

bool Document::deserialize(const QByteArray &data, const QString &projectDir, Project &out, QString *error)
{
    QByteArray raw = data;
    // Accept gzip/zlib-compressed projects too.
    if (raw.size() > 4 && raw[0] != '{') {
        QByteArray u = qUncompress(raw);
        if (!u.isEmpty())
            raw = u;
    }
    QJsonParseError pe;
    QJsonDocument d = QJsonDocument::fromJson(raw, &pe);
    if (d.isNull() || !d.isObject()) {
        if (error) *error = QObject::tr("Invalid project file: %1").arg(pe.errorString());
        return false;
    }
    QJsonObject o = d.object();
    if (o["format"].toString() != "raccord-project") {
        if (error) *error = QObject::tr("Not a Raccord project");
        return false;
    }
    out = Project::fromJson(o);
    out.makePathsAbsolute(projectDir);
    return true;
}

bool Document::load(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    Project p;
    if (!deserialize(f.readAll(), QFileInfo(path).absolutePath(), p, error))
        return false;
    if (p.sequences.empty()) {
        Sequence s;
        s.id = newId();
        s.ensureDefaultTracks();
        p.sequences.push_back(s);
    }
    m_project = std::move(p);
    m_path = QFileInfo(path).absoluteFilePath();
    m_undo.clear();
    m_redo.clear();
    setDirty(false);
    m_autosaveTimer.start(std::max(1, m_project.settings.autosaveMinutes) * 60 * 1000);
    emit historyChanged();
    emit projectReplaced();
    emit changed();
    return true;
}

bool Document::saveCopy(const QString &path, QString *error) const
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(serialize(m_project, QFileInfo(path).absolutePath()));
    if (!f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

bool Document::save(const QString &pathIn, QString *error)
{
    QString path = pathIn.isEmpty() ? m_path : pathIn;
    if (path.isEmpty()) {
        if (error) *error = tr("No file name");
        return false;
    }
    if (!path.endsWith(".raccord"))
        path += ".raccord";
    if (m_project.name == "Untitled" || m_project.name.isEmpty())
        m_project.name = QFileInfo(path).completeBaseName();
    if (!saveCopy(path, error))
        return false;
    m_path = QFileInfo(path).absoluteFilePath();
    setDirty(false);
    emit saved(m_path);
    return true;
}

void Document::autosave()
{
    QString dir = cacheDir() + "/Auto-Save";
    QDir().mkpath(dir);
    QString base = m_path.isEmpty() ? QString("Untitled") : QFileInfo(m_path).completeBaseName();
    QString file = dir + "/" + base + "-" + QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss") + ".raccord";
    saveCopy(file);
    QDir d(dir);
    QFileInfoList list = d.entryInfoList({base + "-*.raccord"}, QDir::Files, QDir::Time);
    for (int i = std::max(1, m_project.settings.autosaveKeep); i < list.size(); ++i)
        QFile::remove(list[i].absoluteFilePath());
}

} // namespace rc
