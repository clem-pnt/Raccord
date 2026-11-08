#pragma once
// Document: owns the Project, undo/redo history, file I/O and autosave.
#include "Model.h"
#include <QObject>
#include <QByteArray>
#include <QTimer>
#include <functional>
#include <deque>

namespace rc {

class Document : public QObject {
    Q_OBJECT
public:
    explicit Document(QObject *parent = nullptr);

    Project &project() { return m_project; }
    const Project &project() const { return m_project; }
    QString path() const { return m_path; }
    QString projectDir() const;
    QString cacheDir() const;   // proxies, waveforms, thumbnails, renders
    bool isDirty() const { return m_dirty; }

    // Applies `fn` as one undoable step. Returns fn's result; a false result rolls back.
    bool edit(const QString &label, const std::function<bool(Project &)> &fn);
    // Convenience: edit on the active sequence.
    bool editSequence(const QString &label, const std::function<bool(Sequence &)> &fn);
    // For continuous interactions (dragging a slider): coalesces consecutive edits with the same key.
    bool editMerged(const QString &label, const QString &mergeKey, const std::function<bool(Project &)> &fn);
    void endMerge() { m_mergeKey.clear(); }
    // Non-undoable changes (playhead, UI state) that still mark the project as modified.
    void touch(bool markDirty = false);

    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    QString undoText() const { return m_undo.empty() ? QString() : m_undo.back().label; }
    QString redoText() const { return m_redo.empty() ? QString() : m_redo.back().label; }
    QStringList history() const;
    void undo();
    void redo();

    void newProject();
    bool load(const QString &path, QString *error = nullptr);
    bool save(const QString &path = {}, QString *error = nullptr);
    bool saveCopy(const QString &path, QString *error = nullptr) const;
    // Autosave into <cache>/Auto-Save, keeping project.settings.autosaveKeep versions.
    void autosave();
    static QByteArray serialize(const Project &p, const QString &projectDir);
    static bool deserialize(const QByteArray &data, const QString &projectDir, Project &out, QString *error = nullptr);

signals:
    void changed();                 // any model change (after edit, undo, redo, load)
    void projectReplaced();         // new / load
    void dirtyChanged(bool dirty);
    void historyChanged();
    void saved(const QString &path);

private:
    struct Step {
        QString label;
        QByteArray before;   // compressed project JSON
        QByteArray after;
    };
    QByteArray snapshot() const;
    void restore(const QByteArray &snap);
    void setDirty(bool d);

    Project m_project;
    QString m_path;
    bool m_dirty = false;
    std::deque<Step> m_undo;
    std::deque<Step> m_redo;
    QString m_mergeKey;
    QTimer m_autosaveTimer;
    int m_maxUndo = 300;
};

} // namespace rc
