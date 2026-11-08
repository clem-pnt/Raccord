#pragma once
#include "Session.h"
#include <QMainWindow>
#include <QPointer>
#include <map>

class QDockWidget;
class QLabel;
class QMenu;
class QListWidget;
class QProgressBar;

namespace rc {

class MonitorPanel;
class TimelinePanel;
class ProjectPanel;
class EffectsBrowser;
class EffectControls;
class ColorPanel;
class ScopesPanel;
class AudioMixerPanel;
class RenderQueue;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;
    bool openProject(const QString &path);
    void importPaths(const QStringList &paths);

protected:
    void closeEvent(QCloseEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    // Construction
    void buildPanels();
    void buildMenus();
    void setWorkspace(const QString &name);
    void saveWorkspace();
    QDockWidget *dock(const QString &id, const QString &title, QWidget *w);
    QAction *action(QMenu *menu, const QString &text, const QKeySequence &key, std::function<void()> fn, const QString &id = {});

    // File
    bool maybeSave();
    void newProject();
    void openDialog();
    bool save();
    bool saveAs();
    void updateTitle();
    void updateRecent(const QString &path);
    void rebuildRecentMenu();
    void checkCrashRecovery();
    void importInterchange();
    void exportInterchange(const QString &ext);
    void exportMedia();
    void exportFrame();
    void importCaptions();
    void exportCaptions();

    // Editing
    void sourceEdit(bool insert);
    void copy(bool cut);
    void paste(bool insert);
    void pasteAttributes();
    void deleteSelection(bool ripple);
    void liftExtract(bool extract);
    void addEdit(bool allTracks);
    void applyDefaultTransition(bool video, bool audio, bool toSelection);
    void speedDialog();
    void audioGainDialog();
    void nestSelection();
    void addMarker();
    void matchFrame();
    void clipMenuAction(const QString &action);
    void markClip();
    void selectAll();
    void stepPlayhead(int frames);
    void goToEdit(bool forward);
    void toggleEnable();

    // Assistant (analysis)
    void runTask(const QString &title, std::function<QString(std::function<bool(double)>)> work, std::function<void(const QString &)> done);
    void transcribe();
    void removeSilences();
    void sceneDetect();
    void autoReframe();
    void stabilize();
    void createProxies();
    void normalizeLoudness();
    // Multicam
    void createMulticam();
    void multicamCut(int angle);
    void multicamClipAction(const QString &what);

    bool sourceFocused() const;
    MonitorPanel *activeMonitor() const;

    MonitorPanel *m_source = nullptr;
    MonitorPanel *m_program = nullptr;
    TimelinePanel *m_timeline = nullptr;
    ProjectPanel *m_project = nullptr;
    EffectsBrowser *m_effects = nullptr;
    EffectControls *m_controls = nullptr;
    ColorPanel *m_color = nullptr;
    ScopesPanel *m_scopes = nullptr;
    AudioMixerPanel *m_mixer = nullptr;
    RenderQueue *m_queue = nullptr;
    QListWidget *m_history = nullptr;
    std::map<QString, QDockWidget *> m_docks;
    QString m_workspace;
    QMenu *m_recentMenu = nullptr;
    QLabel *m_statusInfo = nullptr;
    QProgressBar *m_taskBar = nullptr;
    QByteArray m_clipboard;   // JSON clips
    bool m_taskRunning = false;
};

} // namespace rc
