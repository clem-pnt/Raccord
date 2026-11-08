#include "MainWindow.h"
#include "AudioPanel.h"
#include "ColorPanel.h"
#include "EffectControls.h"
#include "ExportDialog.h"
#include "Icons.h"
#include "MediaCache.h"
#include "Monitor.h"
#include "ProjectPanel.h"
#include "Scopes.h"
#include "Timeline.h"
#include "ai/Analysis.h"
#include "core/Edit.h"
#include "Version.h"
#include "export/Exporter.h"
#include "export/Proxy.h"
#include "interop/Interchange.h"
#include "interop/Subtitles.h"
#include "media/MediaPool.h"
#include "playback/Playback.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace rc {

static Session *S() { return Session::instance(); }

MainWindow::MainWindow()
{
    setWindowIcon(Icons::get("raccord"));
    setDockNestingEnabled(true);
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowTabbedDocks | QMainWindow::AllowNestedDocks | QMainWindow::GroupedDragging);
    buildPanels();
    buildMenus();
    m_statusInfo = new QLabel(this);
    statusBar()->addPermanentWidget(m_statusInfo);
    m_taskBar = new QProgressBar(this);
    m_taskBar->setMaximumWidth(220);
    m_taskBar->setRange(0, 1000);
    m_taskBar->hide();
    statusBar()->addPermanentWidget(m_taskBar);
    connect(S(), &Session::statusMessage, this, [this](const QString &m) { statusBar()->showMessage(m, 4000); });
    connect(&S()->doc(), &Document::dirtyChanged, this, &MainWindow::updateTitle);
    connect(&S()->doc(), &Document::saved, this, [this](const QString &p) { updateTitle(); updateRecent(p); });
    connect(&S()->doc(), &Document::projectReplaced, this, [this] {
        MediaCache::instance()->setCacheDir(S()->doc().cacheDir());
        MediaPool::instance().setCacheDir(S()->doc().cacheDir());
        updateTitle();
    });
    connect(&S()->doc(), &Document::historyChanged, this, [this] {
        if (!m_history) return;
        m_history->clear();
        m_history->addItems(S()->doc().history());
        m_history->scrollToBottom();
    });
    auto updateInfo = [this] {
        const Sequence *seq = S()->sequence();
        if (!seq) return;
        m_statusInfo->setText(QString("%1  |  %2x%3  %4 fps  |  %5").arg(seq->name).arg(seq->width).arg(seq->height)
                                  .arg(seq->fps.fps(), 0, 'f', 3).arg(formatTimecode(seq->duration(), seq->fps)));
    };
    connect(&S()->doc(), &Document::changed, this, updateInfo);
    MediaCache::instance()->setCacheDir(S()->doc().cacheDir());
    MediaPool::instance().setCacheDir(S()->doc().cacheDir());
    QSettings st;
    restoreGeometry(st.value("window/geometry").toByteArray());
    setWorkspace(st.value("window/workspace", "Editing").toString());
    updateTitle();
    updateInfo();
    QTimer::singleShot(0, this, &MainWindow::checkCrashRecovery);
}

MainWindow::~MainWindow() = default;

QDockWidget *MainWindow::dock(const QString &id, const QString &title, QWidget *w)
{
    auto *d = new QDockWidget(title, this);
    d->setObjectName(id);
    d->setWidget(w);
    d->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    m_docks[id] = d;
    return d;
}

void MainWindow::buildPanels()
{
    m_source = new MonitorPanel(Playback::Mode::Source, this);
    m_program = new MonitorPanel(Playback::Mode::Program, this);
    m_timeline = new TimelinePanel(this);
    m_project = new ProjectPanel(this);
    m_effects = new EffectsBrowser(this);
    m_controls = new EffectControls(this);
    m_color = new ColorPanel(this);
    m_scopes = new ScopesPanel(this);
    m_mixer = new AudioMixerPanel(m_program->playback(), this);
    m_queue = new RenderQueue(this);
    m_history = new QListWidget(this);
    setCentralWidget(m_timeline);
    dock("source", tr("Source"), m_source);
    dock("program", tr("Program"), m_program);
    dock("project", tr("Project"), m_project);
    dock("effects", tr("Effects"), m_effects);
    dock("controls", tr("Effect Controls"), m_controls);
    dock("color", tr("Lumetri Color"), m_color);
    dock("scopes", tr("Lumetri Scopes"), m_scopes);
    dock("mixer", tr("Audio Track Mixer"), m_mixer);
    dock("queue", tr("Render Queue"), m_queue);
    dock("history", tr("History"), m_history);

    m_program->view()->setScopesWanted(true);
    connect(m_program->view(), &MonitorView::frameForScopes, m_scopes, &ScopesPanel::setFrame);
    connect(m_source, &MonitorPanel::insertRequested, this, [this] { sourceEdit(true); });
    connect(m_source, &MonitorPanel::overwriteRequested, this, [this] { sourceEdit(false); });
    connect(m_program, &MonitorPanel::exportFrameRequested, this, &MainWindow::exportFrame);
    connect(m_program->view(), &MonitorView::multicamAngleClicked, this, &MainWindow::multicamCut);
    connect(m_program->view(), &MonitorView::droppedFiles, this, [this](const QStringList &paths) {
        m_project->importFiles(paths, {}, [](const QStringList &ids) {
            S()->doc().editSequence(tr("Overwrite"), [&](Sequence &seq) {
                Ticks at = seq.playhead;
                bool any = false;
                for (const auto &id : ids)
                    if (const MediaItem *m = S()->project().mediaItem(id)) {
                        S()->matchSequenceToMedia(seq, *m);
                        auto clips = S()->makeClips(*m, at, 0, 0);
                        if (clips.empty()) continue;
                        any = edit::overwriteEdit(seq, clips) || any;
                        at += clips.front().second.duration;
                    }
                return any;
            });
        });
    });
    connect(m_source->view(), &MonitorView::droppedFiles, this, [this](const QStringList &paths) {
        m_project->importFiles(paths, {}, [](const QStringList &ids) { if (!ids.isEmpty()) S()->setSourceMedia(ids.first()); });
    });
    connect(m_project, &ProjectPanel::openSequence, this, [](const QString &id) { S()->setActiveSequence(id); });
    connect(m_project, &ProjectPanel::status, this, [this](const QString &msg) {
        if (msg == "proxy:create") createProxies();
        else statusBar()->showMessage(msg, 4000);
    });
    connect(m_timeline->view(), &TimelineView::openNested, this, [](const QString &id) { S()->setActiveSequence(id); });
    connect(m_timeline->view(), &TimelineView::requestClipMenuAction, this, &MainWindow::clipMenuAction);
    connect(m_timeline->view(), &TimelineView::requestImport, this, [this](const QStringList &paths, Ticks at, int vt, int at_) {
        m_project->importFiles(paths, {}, [at, vt, at_](const QStringList &ids) {
            S()->doc().editSequence(tr("Overwrite"), [&](Sequence &seq) {
                Ticks pos = at;
                bool any = false;
                for (const auto &id : ids)
                    if (const MediaItem *m = S()->project().mediaItem(id)) {
                        S()->matchSequenceToMedia(seq, *m);
                        auto clips = S()->makeClips(*m, pos, vt, at_);
                        if (clips.empty()) continue;
                        any = edit::overwriteEdit(seq, clips) || any;
                        pos += clips.front().second.duration;
                    }
                return any;
            });
        });
    });
    connect(m_history, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) {
        // Undo back to the clicked step.
        const int target = m_history->row(it);
        const int current = m_history->count() - 1;
        for (int i = current; i > target; --i) S()->doc().undo();
    });
    connect(m_queue, &RenderQueue::jobFinished, this, [this](const QString &path, bool ok, const QString &msg) {
        if (ok) statusBar()->showMessage(tr("Export finished: %1").arg(path), 8000);
        else if (msg != tr("Cancelled")) QMessageBox::warning(this, tr("Export failed"), msg);
    });
    for (QWidget *w : {static_cast<QWidget *>(m_source->view()), static_cast<QWidget *>(m_source)})
        w->installEventFilter(this);
    for (QWidget *w : {static_cast<QWidget *>(m_program->view()), static_cast<QWidget *>(m_program)})
        w->installEventFilter(this);
}

bool MainWindow::eventFilter(QObject *o, QEvent *e)
{
    if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::FocusIn) {
        if (o == m_source || o == m_source->view()) S()->setFocus(Session::Focus::Source);
        else if (o == m_program || o == m_program->view()) S()->setFocus(Session::Focus::Program);
    }
    return QMainWindow::eventFilter(o, e);
}

bool MainWindow::sourceFocused() const { return S()->focus() == Session::Focus::Source; }
MonitorPanel *MainWindow::activeMonitor() const { return sourceFocused() ? m_source : m_program; }

void MainWindow::setWorkspace(const QString &name)
{
    if (!m_workspace.isEmpty())
        saveWorkspace();
    m_workspace = name;
    QSettings st;
    for (auto &[id, d] : m_docks) {
        removeDockWidget(d);
        d->hide();
    }
    const QByteArray saved = st.value("workspace/" + name).toByteArray();
    auto show = [&](const char *id) { m_docks[id]->show(); return m_docks[id]; };
    auto top = [&](const char *id) { addDockWidget(Qt::TopDockWidgetArea, show(id), Qt::Horizontal); return m_docks[id]; };
    auto left = [&](const char *id) { addDockWidget(Qt::LeftDockWidgetArea, show(id)); return m_docks[id]; };
    auto right = [&](const char *id) { addDockWidget(Qt::RightDockWidgetArea, show(id)); return m_docks[id]; };
    if (name == "Color") {
        top("scopes");
        top("program");
        right("color");
        left("project");
        tabifyDockWidget(m_docks["project"], show("effects"));
        resizeDocks({m_docks["scopes"], m_docks["program"]}, {width() / 3, width() / 2}, Qt::Horizontal);
        resizeDocks({m_docks["color"]}, {360}, Qt::Horizontal);
    } else if (name == "Audio") {
        top("mixer");
        top("program");
        left("project");
        tabifyDockWidget(m_docks["project"], show("effects"));
        right("controls");
        resizeDocks({m_docks["mixer"], m_docks["program"]}, {width() / 2, width() / 2}, Qt::Horizontal);
    } else if (name == "Effects") {
        top("controls");
        top("program");
        right("effects");
        left("project");
        tabifyDockWidget(m_docks["project"], show("history"));
        resizeDocks({m_docks["controls"], m_docks["program"]}, {width() / 3, width() / 2}, Qt::Horizontal);
    } else if (name == "Assembly") {
        top("project");
        top("source");
        top("program");
        resizeDocks({m_docks["project"], m_docks["source"], m_docks["program"]}, {width() / 3, width() / 3, width() / 3}, Qt::Horizontal);
    } else {   // Editing
        top("source");
        tabifyDockWidget(m_docks["source"], show("controls"));
        tabifyDockWidget(m_docks["source"], show("mixer"));
        m_docks["source"]->raise();
        top("program");
        left("project");
        tabifyDockWidget(m_docks["project"], show("effects"));
        tabifyDockWidget(m_docks["project"], show("history"));
        tabifyDockWidget(m_docks["project"], show("queue"));
        m_docks["project"]->raise();
        resizeDocks({m_docks["source"], m_docks["program"]}, {width() / 2, width() / 2}, Qt::Horizontal);
        resizeDocks({m_docks["project"]}, {width() / 4}, Qt::Horizontal);
    }
    resizeDocks({m_docks[name == "Color" ? "program" : name == "Audio" ? "mixer" : name == "Effects" ? "controls" : name == "Assembly" ? "project" : "source"]},
                {int(height() * 0.5)}, Qt::Vertical);
    if (!saved.isEmpty())
        restoreState(saved);
    st.setValue("window/workspace", name);
}

void MainWindow::saveWorkspace()
{
    if (!m_workspace.isEmpty())
        QSettings().setValue("workspace/" + m_workspace, saveState());
}

QAction *MainWindow::action(QMenu *menu, const QString &text, const QKeySequence &key, std::function<void()> fn, const QString &id)
{
    QAction *a = menu->addAction(text);
    if (!key.isEmpty()) {
        a->setShortcut(key);
        a->setShortcutContext(Qt::WindowShortcut);
    }
    if (!id.isEmpty()) a->setObjectName(id);
    connect(a, &QAction::triggered, this, [fn] { fn(); });
    addAction(a);   // shortcuts work even with the menu bar hidden / floating panels
    return a;
}

void MainWindow::buildMenus()
{
    using K = QKeySequence;
    Session *s = S();
    // ---------------- File
    QMenu *file = menuBar()->addMenu(tr("&File"));
    action(file, tr("New Project"), K("Ctrl+Alt+N"), [this] { newProject(); });
    action(file, tr("New Sequence..."), K("Ctrl+N"), [this] { m_project->newSequence(); });
    action(file, tr("New Bin"), K("Ctrl+B"), [this] { m_project->newBin(); });
    action(file, tr("Open Project..."), K("Ctrl+O"), [this] { openDialog(); });
    m_recentMenu = file->addMenu(tr("Open Recent"));
    rebuildRecentMenu();
    file->addSeparator();
    action(file, tr("Save"), K("Ctrl+S"), [this] { save(); });
    action(file, tr("Save As..."), K("Ctrl+Shift+S"), [this] { saveAs(); });
    action(file, tr("Save a Copy..."), K("Ctrl+Alt+S"), [this] {
        QString f = QFileDialog::getSaveFileName(this, tr("Save a Copy"), {}, tr("Raccord project (*.raccord)"));
        QString err;
        if (!f.isEmpty() && !S()->doc().saveCopy(f.endsWith(".raccord") ? f : f + ".raccord", &err)) QMessageBox::warning(this, tr("Save"), err);
    });
    action(file, tr("Revert"), {}, [this] {
        const QString p = S()->doc().path();
        if (!p.isEmpty() && QMessageBox::question(this, tr("Revert"), tr("Discard all changes since the last save?")) == QMessageBox::Yes) openProject(p);
    });
    file->addSeparator();
    action(file, tr("Import..."), K("Ctrl+I"), [this] { m_project->importDialog(); });
    action(file, tr("Import Project / XML / EDL / OTIO / Premiere Pro..."), K("Ctrl+Alt+I"), [this] { importInterchange(); });
    action(file, tr("Import Captions (SRT, VTT)..."), {}, [this] { importCaptions(); });
    QMenu *exp = file->addMenu(tr("Export"));
    action(exp, tr("Media..."), K("Ctrl+M"), [this] { exportMedia(); });
    action(exp, tr("Frame..."), K("Ctrl+Shift+E"), [this] { exportFrame(); });
    action(exp, tr("Captions (SRT / VTT)..."), {}, [this] { exportCaptions(); });
    exp->addSeparator();
    action(exp, tr("Final Cut Pro XML (Premiere Pro, Resolve)..."), {}, [this] { exportInterchange("xml"); });
    action(exp, tr("FCPXML (Final Cut Pro X, Resolve)..."), {}, [this] { exportInterchange("fcpxml"); });
    action(exp, tr("EDL (CMX 3600)..."), {}, [this] { exportInterchange("edl"); });
    action(exp, tr("OpenTimelineIO..."), {}, [this] { exportInterchange("otio"); });
    file->addSeparator();
    action(file, tr("Project Settings..."), {}, [this] {
        QDialog dlg(this);
        dlg.setWindowTitle(tr("Project Settings"));
        auto *f = new QFormLayout(&dlg);
        ProjectSettings ps = S()->project().settings;
        auto *proxyH = new QComboBox(&dlg);
        for (int h : {360, 540, 720, 1080}) proxyH->addItem(QString("%1p").arg(h), h);
        proxyH->setCurrentIndex(std::max(0, proxyH->findData(ps.proxyHeight)));
        auto *useProxy = new QCheckBox(tr("Use proxies for playback"), &dlg);
        useProxy->setChecked(ps.useProxies);
        auto *still = new QDoubleSpinBox(&dlg);
        still->setRange(0.1, 600);
        still->setValue(ticksToSeconds(ps.defaultStillDuration));
        still->setSuffix(" s");
        auto *vtr = new QDoubleSpinBox(&dlg);
        vtr->setRange(0.04, 30);
        vtr->setValue(ticksToSeconds(ps.defaultVideoTransition));
        vtr->setSuffix(" s");
        auto *atr = new QDoubleSpinBox(&dlg);
        atr->setRange(0.04, 30);
        atr->setValue(ticksToSeconds(ps.defaultAudioTransition));
        atr->setSuffix(" s");
        auto *autosave = new QSpinBox(&dlg);
        autosave->setRange(1, 120);
        autosave->setValue(ps.autosaveMinutes);
        autosave->setSuffix(" min");
        auto *scratch = new QLineEdit(ps.scratchDir, &dlg);
        scratch->setPlaceholderText(tr("Next to the project"));
        f->addRow(tr("Proxy resolution"), proxyH);
        f->addRow(useProxy);
        f->addRow(tr("Still image duration"), still);
        f->addRow(tr("Video transition duration"), vtr);
        f->addRow(tr("Audio transition duration"), atr);
        f->addRow(tr("Auto save every"), autosave);
        f->addRow(tr("Cache / scratch folder"), scratch);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        f->addRow(bb);
        connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted) return;
        S()->doc().edit(tr("Project Settings"), [&](Project &p) {
            p.settings.proxyHeight = proxyH->currentData().toInt();
            p.settings.useProxies = useProxy->isChecked();
            p.settings.defaultStillDuration = secondsToTicks(still->value());
            p.settings.defaultVideoTransition = secondsToTicks(vtr->value());
            p.settings.defaultAudioTransition = secondsToTicks(atr->value());
            p.settings.autosaveMinutes = autosave->value();
            p.settings.scratchDir = scratch->text();
            return true;
        });
    });
    file->addSeparator();
    action(file, tr("Quit"), K("Ctrl+Q"), [this] { close(); });

    // ---------------- Edit
    QMenu *ed = menuBar()->addMenu(tr("&Edit"));
    QAction *undo = action(ed, tr("Undo"), K("Ctrl+Z"), [s] { s->doc().undo(); });
    QAction *redo = action(ed, tr("Redo"), K("Ctrl+Shift+Z"), [s] { s->doc().redo(); });
    connect(&s->doc(), &Document::historyChanged, this, [undo, redo, s] {
        undo->setEnabled(s->doc().canUndo());
        redo->setEnabled(s->doc().canRedo());
        undo->setText(s->doc().canUndo() ? tr("Undo %1").arg(s->doc().undoText()) : tr("Undo"));
        redo->setText(s->doc().canRedo() ? tr("Redo %1").arg(s->doc().redoText()) : tr("Redo"));
    });
    ed->addSeparator();
    action(ed, tr("Cut"), K("Ctrl+X"), [this] { copy(true); });
    action(ed, tr("Copy"), K("Ctrl+C"), [this] { copy(false); });
    action(ed, tr("Paste"), K("Ctrl+V"), [this] { paste(false); });
    action(ed, tr("Paste Insert"), K("Ctrl+Shift+V"), [this] { paste(true); });
    action(ed, tr("Paste Attributes..."), K("Ctrl+Alt+V"), [this] { pasteAttributes(); });
    action(ed, tr("Clear"), K(Qt::Key_Delete), [this] { deleteSelection(false); });
    action(ed, tr("Ripple Delete"), K("Shift+Delete"), [this] { deleteSelection(true); });
    action(ed, tr("Duplicate"), K("Ctrl+Shift+/"), [s] {
        std::vector<QString> ids(s->selection().begin(), s->selection().end());
        if (ids.empty()) return;
        s->doc().editSequence(tr("Duplicate"), [&](Sequence &seq) {
            Ticks len = 0;
            for (const auto &id : ids) if (const Clip *c = seq.clip(id)) len = std::max(len, c->end());
            Ticks first = INT64_MAX;
            for (const auto &id : ids) if (const Clip *c = seq.clip(id)) first = std::min(first, c->start);
            return !edit::duplicateClips(seq, ids, len - first, 0, 0).empty();
        });
    });
    ed->addSeparator();
    action(ed, tr("Select All"), K("Ctrl+A"), [this] { selectAll(); });
    action(ed, tr("Deselect All"), K("Ctrl+Shift+A"), [s] { s->clearSelection(); });
    ed->addSeparator();
    action(ed, tr("Keyboard Shortcuts..."), K("Ctrl+Alt+K"), [this] {
        QDialog dlg(this);
        dlg.setWindowTitle(tr("Keyboard Shortcuts"));
        dlg.resize(520, 600);
        auto *v = new QVBoxLayout(&dlg);
        auto *list = new QPlainTextEdit(&dlg);
        list->setReadOnly(true);
        QStringList lines;
        for (QAction *a : actions())
            if (!a->shortcut().isEmpty())
                lines << QString("%1\t%2").arg(a->shortcut().toString(QKeySequence::NativeText), -18).arg(a->text().remove('&'));
        lines.sort();
        list->setPlainText(lines.join('\n'));
        v->addWidget(list);
        dlg.exec();
    });

    // ---------------- Clip
    QMenu *clip = menuBar()->addMenu(tr("&Clip"));
    action(clip, tr("Speed/Duration..."), K("Ctrl+R"), [this] { speedDialog(); });
    action(clip, tr("Audio Gain..."), K("G"), [this] { audioGainDialog(); });
    action(clip, tr("Normalize Loudness..."), {}, [this] { normalizeLoudness(); });
    action(clip, tr("Enable"), K("Shift+E"), [this] { toggleEnable(); });
    action(clip, tr("Link"), K("Ctrl+L"), [s] {
        std::vector<QString> ids(s->selection().begin(), s->selection().end());
        bool linked = false;
        if (const Sequence *seq = s->sequence())
            for (const auto &id : ids) if (const Clip *c = seq->clip(id); c && !c->linkId.isEmpty()) linked = true;
        s->doc().editSequence(linked ? tr("Unlink") : tr("Link"), [&](Sequence &seq) {
            if (linked) edit::unlink(seq, ids); else edit::link(seq, ids);
            return true;
        });
    });
    action(clip, tr("Group"), K("Ctrl+G"), [s] {
        std::vector<QString> ids(s->selection().begin(), s->selection().end());
        s->doc().editSequence(tr("Group"), [&](Sequence &seq) { edit::group(seq, ids); return true; });
    });
    action(clip, tr("Ungroup"), K("Ctrl+Shift+G"), [s] {
        std::vector<QString> ids(s->selection().begin(), s->selection().end());
        s->doc().editSequence(tr("Ungroup"), [&](Sequence &seq) { edit::ungroup(seq, ids); return true; });
    });
    action(clip, tr("Nest..."), {}, [this] { nestSelection(); });
    clip->addSeparator();
    action(clip, tr("Set to Frame Size"), {}, [this] { clipMenuAction("fit"); });
    action(clip, tr("Scale to Frame Size (Fill)"), {}, [this] { clipMenuAction("fill"); });
    action(clip, tr("Make Subclip"), K("Ctrl+U"), [this] { clipMenuAction("subclip"); });
    clip->addSeparator();
    QMenu *mc = clip->addMenu(tr("Multi-Camera"));
    action(mc, tr("Create Multi-Camera Source Sequence..."), {}, [this] { createMulticam(); });
    QAction *mcView = action(mc, tr("Multi-Camera View"), K("Shift+0"), [this] {
        m_program->view()->setMulticamView(!m_program->view()->multicamView());
    });
    mcView->setCheckable(true);
    connect(mc, &QMenu::aboutToShow, mcView, [this, mcView] { mcView->setChecked(m_program->view()->multicamView()); });
    action(mc, tr("Audio Follows Video"), {}, [this] { multicamClipAction("audioFollows"); });
    action(mc, tr("Flatten"), {}, [this] { multicamClipAction("flatten"); });
    mc->addSeparator();
    for (int k = 0; k < 9; ++k)
        action(mc, tr("Cut to Camera %1").arg(k + 1), K(QString::number(k + 1)), [this, k] { multicamCut(k); });

    // ---------------- Sequence
    QMenu *seqm = menuBar()->addMenu(tr("&Sequence"));
    action(seqm, tr("Add Edit"), K("Ctrl+K"), [this] { addEdit(false); });
    action(seqm, tr("Add Edit to All Tracks"), K("Ctrl+Shift+K"), [this] { addEdit(true); });
    action(seqm, tr("Apply Video Transition"), K("Ctrl+D"), [this] { applyDefaultTransition(true, false, false); });
    action(seqm, tr("Apply Audio Transition"), K("Ctrl+Shift+D"), [this] { applyDefaultTransition(false, true, false); });
    action(seqm, tr("Apply Default Transitions to Selection"), K("Shift+D"), [this] { applyDefaultTransition(true, true, true); });
    seqm->addSeparator();
    action(seqm, tr("Insert"), K(","), [this] { sourceEdit(true); });
    action(seqm, tr("Overwrite"), K("."), [this] { sourceEdit(false); });
    action(seqm, tr("Lift"), K(";"), [this] { liftExtract(false); });
    action(seqm, tr("Extract"), K("'"), [this] { liftExtract(true); });
    action(seqm, tr("Ripple Delete Gap / Close Gap"), {}, [s] {
        s->doc().editSequence(tr("Close Gap"), [&](Sequence &seq) {
            bool any = false;
            for (const auto &r : s->targetedTracks()) any = edit::closeGap(seq, r, seq.playhead) || any;
            return any;
        });
    });
    action(seqm, tr("Close All Gaps"), {}, [s] { s->doc().editSequence(tr("Close Gaps"), [&](Sequence &seq) { edit::closeAllGaps(seq); return true; }); });
    action(seqm, tr("Match Frame"), K("F"), [this] { matchFrame(); });
    action(seqm, tr("Snap in Timeline"), K("S"), [s] { s->setSnapping(!s->snapping()); });
    action(seqm, tr("Linked Selection"), {}, [s] { s->setLinkedSelection(!s->linkedSelection()); });
    seqm->addSeparator();
    action(seqm, tr("Zoom In"), K("="), [this] { m_timeline->view()->zoomIn(); });
    action(seqm, tr("Zoom Out"), K("-"), [this] { m_timeline->view()->zoomOut(); });
    action(seqm, tr("Zoom to Sequence"), K("\\"), [this] { m_timeline->view()->zoomToFit(); });
    seqm->addSeparator();
    action(seqm, tr("Add Video Track"), {}, [s] { s->doc().editSequence(tr("Add Track"), [](Sequence &q) { q.addTrack(TrackType::Video); q.renameTracks(); return true; }); });
    action(seqm, tr("Add Audio Track"), {}, [s] { s->doc().editSequence(tr("Add Track"), [](Sequence &q) { q.addTrack(TrackType::Audio); q.renameTracks(); return true; }); });
    action(seqm, tr("Add Caption Track"), {}, [s] {
        s->doc().editSequence(tr("Add Caption Track"), [](Sequence &q) {
            CaptionTrack t;
            t.id = newId();
            t.name = tr("Subtitles %1").arg(q.captionTracks.size() + 1);
            q.captionTracks.push_back(t);
            return true;
        });
    });
    action(seqm, tr("Add Caption at Playhead"), K("Ctrl+Alt+C"), [s] {
        s->doc().editSequence(tr("Add Caption"), [](Sequence &q) {
            if (q.captionTracks.empty()) { CaptionTrack t; t.id = newId(); q.captionTracks.push_back(t); }
            Caption c;
            c.id = newId();
            c.start = q.playhead;
            c.end = q.playhead + 3 * kTicksPerSecond;
            c.text = tr("New caption");
            q.captionTracks.front().captions.push_back(c);
            q.captionTracks.front().sort();
            return true;
        });
    });
    action(seqm, tr("Auto Reframe Sequence (9:16, 1:1, 4:5)..."), {}, [this] {
        bool ok = false;
        const QStringList choices = {"9:16 (1080x1920)", "1:1 (1080x1080)", "4:5 (1080x1350)", "16:9 (1920x1080)"};
        QString c = QInputDialog::getItem(this, tr("Auto Reframe Sequence"), tr("Target aspect ratio:"), choices, 0, false, &ok);
        if (!ok) return;
        const int idx = choices.indexOf(c);
        const int w[] = {1080, 1080, 1080, 1920}, h[] = {1920, 1080, 1350, 1080};
        const Sequence *cur = S()->sequence();
        if (!cur) return;
        QString id = newId();
        S()->doc().edit(tr("Auto Reframe Sequence"), [&](Project &p) {
            Sequence copy = *p.activeSequence();
            copy.id = id;
            copy.name += QString(" - %1").arg(c.section(' ', 0, 0));
            copy.width = w[idx];
            copy.height = h[idx];
            p.sequences.push_back(copy);
            p.activeSequenceId = id;
            return true;
        });
        S()->setActiveSequence(id);
        QSet<QString> all;
        for (const auto &t : S()->sequence()->videoTracks)
            for (const auto &cl : t.clips) if (cl.source == ClipSource::Media) all.insert(cl.id);
        S()->select(all, false);
        autoReframe();
    });
    action(seqm, tr("Sequence Settings..."), {}, [this] {
        const Sequence *sq = S()->sequence();
        if (!sq) return;
        bool ok = false;
        QString size = QInputDialog::getText(this, tr("Sequence Settings"), tr("Frame size (WxH):"), QLineEdit::Normal, QString("%1x%2").arg(sq->width).arg(sq->height), &ok);
        const QStringList wh = size.split('x');
        if (!ok || wh.size() != 2) return;
        S()->doc().editSequence(tr("Sequence Settings"), [&](Sequence &q) {
            q.width = std::max(16, wh[0].trimmed().toInt()) & ~1;
            q.height = std::max(16, wh[1].trimmed().toInt()) & ~1;
            return true;
        });
    });

    // ---------------- Markers
    QMenu *mk = menuBar()->addMenu(tr("&Markers"));
    action(mk, tr("Mark In"), K("I"), [this] { activeMonitor()->markIn(); });
    action(mk, tr("Mark Out"), K("O"), [this] { activeMonitor()->markOut(); });
    action(mk, tr("Mark Clip"), K("X"), [this] { markClip(); });
    action(mk, tr("Clear In and Out"), K("Ctrl+Shift+X"), [this] { activeMonitor()->clearInOut(); });
    action(mk, tr("Go to In"), K("Shift+I"), [this] { activeMonitor()->goToIn(); });
    action(mk, tr("Go to Out"), K("Shift+O"), [this] { activeMonitor()->goToOut(); });
    mk->addSeparator();
    action(mk, tr("Add Marker"), K("M"), [this] { addMarker(); });
    action(mk, tr("Go to Next Marker"), K("Shift+M"), [s] {
        const Sequence *q = s->sequence();
        if (!q) return;
        Ticks best = INT64_MAX;
        for (const auto &m : q->markers) if (m.time > q->playhead) best = std::min(best, m.time);
        if (best != INT64_MAX) s->setPlayhead(best);
    });
    action(mk, tr("Go to Previous Marker"), K("Ctrl+Shift+M"), [s] {
        const Sequence *q = s->sequence();
        if (!q) return;
        Ticks best = -1;
        for (const auto &m : q->markers) if (m.time < q->playhead) best = std::max(best, m.time);
        if (best >= 0) s->setPlayhead(best);
    });
    action(mk, tr("Clear Selected Marker"), K("Ctrl+Alt+M"), [s] {
        s->doc().editSequence(tr("Clear Marker"), [](Sequence &q) {
            auto it = std::find_if(q.markers.begin(), q.markers.end(), [&](const Marker &m) { return m.time == q.playhead; });
            if (it == q.markers.end()) return false;
            q.markers.erase(it);
            return true;
        });
    });
    action(mk, tr("Clear All Markers"), K("Ctrl+Alt+Shift+M"), [s] { s->doc().editSequence(tr("Clear Markers"), [](Sequence &q) { q.markers.clear(); return true; }); });

    // ---------------- Playback / navigation (Premiere keys)
    QMenu *pb = menuBar()->addMenu(tr("&Playback"));
    action(pb, tr("Play/Stop"), K(Qt::Key_Space), [this] { activeMonitor()->playback()->toggle(); });
    action(pb, tr("Shuttle Left"), K("J"), [this] { activeMonitor()->playback()->shuttleBackward(); });
    action(pb, tr("Stop"), K("K"), [this] { activeMonitor()->playback()->stop(); });
    action(pb, tr("Shuttle Right"), K("L"), [this] { activeMonitor()->playback()->shuttleForward(); });
    action(pb, tr("Play In to Out"), K("Ctrl+Shift+Space"), [this] {
        MonitorPanel *m = activeMonitor();
        m->goToIn();
        m->playback()->play(1.0);
    });
    action(pb, tr("Loop"), K("Ctrl+Alt+L"), [this] { auto *p = activeMonitor()->playback(); p->setLoop(!p->loop()); });
    pb->addSeparator();
    action(pb, tr("Step Back"), K(Qt::Key_Left), [this] { stepPlayhead(-1); });
    action(pb, tr("Step Forward"), K(Qt::Key_Right), [this] { stepPlayhead(1); });
    action(pb, tr("Step Back 5 Frames"), K("Shift+Left"), [this] { stepPlayhead(-5); });
    action(pb, tr("Step Forward 5 Frames"), K("Shift+Right"), [this] { stepPlayhead(5); });
    action(pb, tr("Go to Previous Edit"), K(Qt::Key_Up), [this] { goToEdit(false); });
    action(pb, tr("Go to Next Edit"), K(Qt::Key_Down), [this] { goToEdit(true); });
    action(pb, tr("Go to Start"), K(Qt::Key_Home), [this] {
        if (sourceFocused()) S()->setSourcePosition(0); else S()->setPlayhead(0);
    });
    action(pb, tr("Go to End"), K(Qt::Key_End), [this] {
        if (sourceFocused()) { if (const MediaItem *m = S()->project().mediaItem(S()->sourceMedia())) S()->setSourcePosition(m->info.duration); }
        else if (const Sequence *q = S()->sequence()) S()->setPlayhead(q->duration());
    });
    action(pb, tr("Trim Previous Edit to Playhead"), K("Q"), [s] {
        s->doc().editSequence(tr("Ripple Trim"), [&](Sequence &q) {
            bool any = false;
            const auto len = s->sourceLength();
            for (const auto &r : s->targetedTracks())
                for (const auto &c : q.tracks(r.type)[r.index].clips)
                    if (c.start < q.playhead && c.end() > q.playhead) { any = edit::trimIn(q, c.id, q.playhead, edit::TrimMode::Ripple, len) || any; break; }
            return any;
        });
    });
    action(pb, tr("Trim Next Edit to Playhead"), K("W"), [s] {
        s->doc().editSequence(tr("Ripple Trim"), [&](Sequence &q) {
            bool any = false;
            const auto len = s->sourceLength();
            for (const auto &r : s->targetedTracks())
                for (const auto &c : q.tracks(r.type)[r.index].clips)
                    if (c.start < q.playhead && c.end() > q.playhead) { any = edit::trimOut(q, c.id, q.playhead, edit::TrimMode::Ripple, len) || any; break; }
            return any;
        });
    });

    // ---------------- Tools
    QMenu *tools = menuBar()->addMenu(tr("&Tools"));
    const std::pair<QString, std::pair<Tool, const char *>> toolKeys[] = {
        {tr("Selection Tool"), {Tool::Selection, "V"}}, {tr("Track Select Forward Tool"), {Tool::TrackSelectForward, "A"}},
        {tr("Track Select Backward Tool"), {Tool::TrackSelectBackward, "Shift+A"}}, {tr("Ripple Edit Tool"), {Tool::Ripple, "B"}},
        {tr("Rolling Edit Tool"), {Tool::Rolling, "N"}}, {tr("Rate Stretch Tool"), {Tool::RateStretch, "R"}}, {tr("Razor Tool"), {Tool::Razor, "C"}},
        {tr("Slip Tool"), {Tool::Slip, "Y"}}, {tr("Slide Tool"), {Tool::Slide, "U"}}, {tr("Pen Tool"), {Tool::Pen, "P"}},
        {tr("Hand Tool"), {Tool::Hand, "H"}}, {tr("Zoom Tool"), {Tool::Zoom, "Z"}}, {tr("Type Tool"), {Tool::Type, "T"}},
    };
    for (const auto &[name, tk] : toolKeys) {
        const Tool t = tk.first;
        action(tools, name, K(tk.second), [s, t] { s->setTool(t); });
    }

    // ---------------- Assistant
    QMenu *ai = menuBar()->addMenu(tr("&Assistant"));
    action(ai, tr("Transcribe to Captions (Whisper)..."), {}, [this] { transcribe(); });
    action(ai, tr("Remove Silences..."), {}, [this] { removeSilences(); });
    action(ai, tr("Scene Edit Detection"), {}, [this] { sceneDetect(); });
    action(ai, tr("Auto Reframe Selected Clips"), {}, [this] { autoReframe(); });
    action(ai, tr("Warp Stabilizer (Analyze)"), {}, [this] { stabilize(); });
    action(ai, tr("Create Proxies"), {}, [this] { createProxies(); });
    action(ai, tr("Install Transcription Engine..."), {}, [this] {
        runTask(tr("Installing transcription engine"), [](std::function<bool(double)> pr) {
            QString err;
            pr(0.1);
            if (!ai::installWhisper({}, &err)) return err;
            return QString();
        }, [this](const QString &err) {
            if (err.isEmpty()) QMessageBox::information(this, tr("Transcription"), tr("The transcription engine is installed."));
            else QMessageBox::warning(this, tr("Transcription"), err);
        });
    });

    // ---------------- Window
    QMenu *win = menuBar()->addMenu(tr("&Window"));
    QMenu *ws = win->addMenu(tr("Workspaces"));
    int n = 1;
    for (const QString &w : {QString("Editing"), QString("Color"), QString("Audio"), QString("Effects"), QString("Assembly")}) {
        action(ws, tr(w.toUtf8().constData()), K(QString("Alt+Shift+%1").arg(n++)), [this, w] { setWorkspace(w); });
    }
    ws->addSeparator();
    action(ws, tr("Reset to Saved Layout"), {}, [this] {
        QSettings().remove("workspace/" + m_workspace);
        const QString w = m_workspace;
        m_workspace.clear();
        setWorkspace(w);
    });
    win->addSeparator();
    const std::pair<const char *, const char *> panelKeys[] = {{"project", "Shift+1"}, {"source", "Shift+2"}, {"program", "Shift+4"}, {"controls", "Shift+5"},
                                                               {"mixer", "Shift+6"}, {"effects", "Shift+7"}, {"color", ""}, {"scopes", ""}, {"queue", ""}, {"history", ""}};
    for (const auto &[id, key] : panelKeys) {
        QDockWidget *d = m_docks[id];
        QAction *a = action(win, d->windowTitle(), K(key), [d] { d->show(); d->raise(); d->widget()->setFocus(); });
        (void)a;
    }
    action(win, tr("Timeline"), K("Shift+3"), [this] { m_timeline->view()->setFocus(); S()->setFocus(Session::Focus::Timeline); });
    action(win, tr("Full Screen"), K("Ctrl+`"), [this] { isFullScreen() ? showNormal() : showFullScreen(); });

    // ---------------- Help
    QMenu *help = menuBar()->addMenu(tr("&Help"));
    action(help, tr("About Raccord"), {}, [this] {
        QMessageBox::about(this, tr("About Raccord"),
                           tr("<h2>Raccord %1</h2><p>Professional video editing for Linux.</p>"
                              "<p>GPU rendering, Premiere-style editing, colour grading, audio mixing, captions and assistant tools.</p>")
                               .arg(RACCORD_VERSION));
    });
}

// ---------------------------------------------------------------------------
// File

void MainWindow::updateTitle()
{
    const QString p = S()->doc().path();
    setWindowTitle(QString("%1%2 - Raccord").arg(p.isEmpty() ? tr("Untitled") : QFileInfo(p).completeBaseName()).arg(S()->doc().isDirty() ? " *" : ""));
}

bool MainWindow::maybeSave()
{
    if (!S()->doc().isDirty())
        return true;
    auto r = QMessageBox::question(this, tr("Save Changes"), tr("Save changes to the current project?"),
                                   QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) return save();
    return true;
}

void MainWindow::newProject()
{
    if (!maybeSave()) return;
    m_program->playback()->stop();
    m_source->playback()->stop();
    S()->doc().newProject();
}

void MainWindow::openDialog()
{
    if (!maybeSave()) return;
    QString f = QFileDialog::getOpenFileName(this, tr("Open Project"), QSettings().value("recent/dir").toString(),
                                             tr("Projects (*.raccord *.prproj *.xml *.otio *.edl);;All files (*)"));
    if (!f.isEmpty()) openProject(f);
}

bool MainWindow::openProject(const QString &path)
{
    m_program->playback()->stop();
    m_source->playback()->stop();
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext != "raccord") {
        // Foreign project: import into a fresh project.
        S()->doc().newProject();
        QString err;
        QStringList ids;
        const QString seqId = S()->project().activeSequenceId;
        S()->doc().edit(tr("Import Project"), [&](Project &p) {
            ids = interop::importAny(path, p, p.activeSequence() ? p.activeSequence()->fps : FrameRate{25, 1}, &err);
            if (ids.isEmpty()) return false;
            // Drop the empty default sequence.
            p.sequences.erase(std::remove_if(p.sequences.begin(), p.sequences.end(), [&](const Sequence &q) { return q.id == seqId && q.duration() == 0; }), p.sequences.end());
            p.activeSequenceId = ids.first();
            p.name = QFileInfo(path).completeBaseName();
            return true;
        });
        if (ids.isEmpty()) {
            QMessageBox::warning(this, tr("Open"), tr("Could not import %1:\n%2").arg(path, err));
            return false;
        }
        S()->setActiveSequence(ids.first());
        emit S()->sequenceChanged();
        statusBar()->showMessage(tr("Imported %1").arg(QFileInfo(path).fileName()), 5000);
        return true;
    }
    QString err;
    if (!S()->doc().load(path, &err)) {
        QMessageBox::warning(this, tr("Open Project"), tr("Could not open %1:\n%2").arg(path, err));
        return false;
    }
    updateRecent(path);
    // Report offline media.
    int offline = 0;
    for (const auto &m : S()->project().media) if (!m.path.isEmpty() && !QFileInfo::exists(m.path.section('%', 0, 0).isEmpty() ? m.path : m.path) && !m.info.isImageSequence) ++offline;
    if (offline) statusBar()->showMessage(tr("%n media file(s) offline - use Relink Media in the Project panel", "", offline), 10000);
    return true;
}

bool MainWindow::save()
{
    if (S()->doc().path().isEmpty()) return saveAs();
    QString err;
    if (!S()->doc().save({}, &err)) {
        QMessageBox::warning(this, tr("Save"), err);
        return false;
    }
    statusBar()->showMessage(tr("Saved"), 2000);
    return true;
}

bool MainWindow::saveAs()
{
    QString f = QFileDialog::getSaveFileName(this, tr("Save Project As"), QSettings().value("recent/dir").toString() + "/" + S()->project().name + ".raccord",
                                             tr("Raccord project (*.raccord)"));
    if (f.isEmpty()) return false;
    if (!f.endsWith(".raccord")) f += ".raccord";
    QString err;
    if (!S()->doc().save(f, &err)) {
        QMessageBox::warning(this, tr("Save"), err);
        return false;
    }
    MediaCache::instance()->setCacheDir(S()->doc().cacheDir());
    MediaPool::instance().setCacheDir(S()->doc().cacheDir());
    return true;
}

void MainWindow::updateRecent(const QString &path)
{
    QSettings st;
    QStringList r = st.value("recent/files").toStringList();
    r.removeAll(path);
    r.prepend(path);
    while (r.size() > 12) r.removeLast();
    st.setValue("recent/files", r);
    st.setValue("recent/dir", QFileInfo(path).absolutePath());
    rebuildRecentMenu();
}

void MainWindow::rebuildRecentMenu()
{
    if (!m_recentMenu) return;
    m_recentMenu->clear();
    for (const QString &f : QSettings().value("recent/files").toStringList())
        m_recentMenu->addAction(f, this, [this, f] { if (maybeSave()) openProject(f); });
    if (m_recentMenu->isEmpty()) m_recentMenu->addAction(tr("(none)"))->setEnabled(false);
}

void MainWindow::checkCrashRecovery()
{
    QSettings st;
    const bool clean = st.value("session/clean", true).toBool();
    st.setValue("session/clean", false);
    st.sync();
    if (clean) return;
    // Look for the most recent autosave.
    QString best;
    QDateTime bestTime;
    QStringList dirs = {QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/unsaved/Auto-Save"};
    const QString last = st.value("recent/files").toStringList().value(0);
    if (!last.isEmpty()) dirs << QFileInfo(last).absolutePath() + "/" + QFileInfo(last).completeBaseName() + " Cache/Auto-Save";
    for (const QString &d : dirs) {
        QDirIterator it(d, {"*.raccord"}, QDir::Files);
        while (it.hasNext()) {
            it.next();
            if (!bestTime.isValid() || it.fileInfo().lastModified() > bestTime) { bestTime = it.fileInfo().lastModified(); best = it.filePath(); }
        }
    }
    if (best.isEmpty()) return;
    if (QMessageBox::question(this, tr("Recover"), tr("Raccord did not close properly. Restore the auto-saved project from %1?")
                                                      .arg(QLocale().toString(bestTime, QLocale::ShortFormat))) == QMessageBox::Yes) {
        QString err;
        if (!S()->doc().load(best, &err)) QMessageBox::warning(this, tr("Recover"), err);
    }
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if (m_queue->busy() && QMessageBox::question(this, tr("Quit"), tr("An export is running. Quit anyway?")) != QMessageBox::Yes) {
        e->ignore();
        return;
    }
    if (!maybeSave()) {
        e->ignore();
        return;
    }
    m_program->playback()->stop();
    m_source->playback()->stop();
    saveWorkspace();
    QSettings st;
    st.setValue("window/geometry", saveGeometry());
    st.setValue("session/clean", true);
    e->accept();
}

void MainWindow::importPaths(const QStringList &paths)
{
    QStringList media;
    for (const QString &p : paths) {
        const QString ext = QFileInfo(p).suffix().toLower();
        if (ext == "raccord" || ext == "prproj" || ext == "otio" || ext == "edl") openProject(p);
        else media << p;
    }
    if (!media.isEmpty()) m_project->importFiles(media);
}

void MainWindow::importInterchange()
{
    QString f = QFileDialog::getOpenFileName(this, tr("Import"), {}, tr("Interchange (*.xml *.edl *.otio *.prproj);;All files (*)"));
    if (f.isEmpty()) return;
    QString err;
    QStringList ids;
    S()->doc().edit(tr("Import %1").arg(QFileInfo(f).fileName()), [&](Project &p) {
        ids = interop::importAny(f, p, S()->sequence() ? S()->sequence()->fps : FrameRate{25, 1}, &err);
        return !ids.isEmpty();
    });
    if (ids.isEmpty()) QMessageBox::warning(this, tr("Import"), err.isEmpty() ? tr("Nothing to import") : err);
    else S()->setActiveSequence(ids.first());
}

void MainWindow::exportInterchange(const QString &ext)
{
    const Sequence *seq = S()->sequence();
    if (!seq) return;
    QString f = QFileDialog::getSaveFileName(this, tr("Export"), QSettings().value("recent/dir").toString() + "/" + seq->name + "." + ext, "*." + ext);
    if (f.isEmpty()) return;
    if (!f.endsWith("." + ext)) f += "." + ext;
    QString err;
    if (!interop::exportAny(S()->project(), *seq, f, &err)) QMessageBox::warning(this, tr("Export"), err);
    else statusBar()->showMessage(tr("Exported %1").arg(f), 5000);
}

void MainWindow::exportMedia()
{
    const Sequence *seq = S()->sequence();
    if (!seq) return;
    if (seq->duration() == 0) {
        QMessageBox::information(this, tr("Export"), tr("The sequence is empty."));
        return;
    }
    ExportDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;
    ExportSettings s = dlg.settings();
    QSettings().setValue("export/dir", QFileInfo(s.path).absolutePath());
    m_queue->add(seq->id, s, !dlg.queueOnly());
    m_docks["queue"]->show();
    m_docks["queue"]->raise();
}

void MainWindow::exportFrame()
{
    MonitorPanel *mon = sourceFocused() ? m_source : m_program;
    QImage img = mon->view()->grabCurrent();
    if (img.isNull()) return;
    const Sequence *seq = S()->sequence();
    QString base = (seq ? seq->name : QString("frame")) + "_" + (seq ? formatTimecode(seq->playhead, seq->fps).replace(':', '-').replace(';', '-') : QString());
    QString f = QFileDialog::getSaveFileName(this, tr("Export Frame"), QSettings().value("export/dir").toString() + "/" + base + ".png",
                                             tr("PNG (*.png);;JPEG (*.jpg);;TIFF (*.tif)"));
    if (f.isEmpty()) return;
    if (!img.save(f)) QMessageBox::warning(this, tr("Export Frame"), tr("Could not write %1").arg(f));
}

void MainWindow::importCaptions()
{
    QString f = QFileDialog::getOpenFileName(this, tr("Import Captions"), {}, tr("Subtitles (*.srt *.vtt)"));
    if (f.isEmpty()) return;
    CaptionTrack t;
    QString err;
    if (!readSubtitles(f, t, &err)) {
        QMessageBox::warning(this, tr("Import Captions"), err);
        return;
    }
    t.id = newId();
    t.name = QFileInfo(f).completeBaseName();
    S()->doc().editSequence(tr("Import Captions"), [&](Sequence &q) { q.captionTracks.push_back(t); return true; });
}

void MainWindow::exportCaptions()
{
    const Sequence *seq = S()->sequence();
    if (!seq || seq->captionTracks.empty()) {
        QMessageBox::information(this, tr("Export Captions"), tr("The sequence has no caption track."));
        return;
    }
    QString f = QFileDialog::getSaveFileName(this, tr("Export Captions"), seq->name + ".srt", tr("SubRip (*.srt);;WebVTT (*.vtt)"));
    if (f.isEmpty()) return;
    const bool ok = f.endsWith(".vtt") ? writeVtt(seq->captionTracks.front(), f) : writeSrt(seq->captionTracks.front(), f.endsWith(".srt") ? f : f + ".srt");
    if (!ok) QMessageBox::warning(this, tr("Export Captions"), tr("Could not write %1").arg(f));
}

// ---------------------------------------------------------------------------
// Editing

void MainWindow::sourceEdit(bool insert)
{
    Session *s = S();
    const MediaItem *m = s->project().mediaItem(s->sourceMedia());
    if (!m) {
        statusBar()->showMessage(tr("Open a clip in the Source monitor first"), 3000);
        return;
    }
    const MediaItem item = *m;
    int vt = -1, at = -1;
    for (const auto &r : s->targetedTracks()) {
        if (r.type == TrackType::Video && vt < 0) vt = r.index;
        if (r.type == TrackType::Audio && at < 0) at = r.index;
    }
    if (vt < 0 && at < 0) {
        statusBar()->showMessage(tr("No target track"), 3000);
        return;
    }
    s->doc().editSequence(insert ? tr("Insert") : tr("Overwrite"), [&](Sequence &seq) {
        s->matchSequenceToMedia(seq, item);
        Ticks at0 = seq.inPoint != kInvalidTicks ? seq.inPoint : seq.playhead;
        auto clips = s->makeClips(item, at0, std::max(0, vt), std::max(0, at), vt >= 0, at >= 0);
        if (clips.empty()) return false;
        const Ticks dur = clips.front().second.duration;
        const bool ok = insert ? edit::insertEdit(seq, clips, at0, dur) : edit::overwriteEdit(seq, clips);
        if (ok) seq.playhead = at0 + dur;
        return ok;
    });
}

void MainWindow::copy(bool cut)
{
    Session *s = S();
    const Sequence *seq = s->sequence();
    if (!seq || s->selection().isEmpty()) return;
    QJsonArray arr;
    Ticks first = INT64_MAX;
    for (const auto &id : s->selection()) if (const Clip *c = seq->clip(id)) first = std::min(first, c->start);
    for (const auto &id : s->selection()) {
        auto r = seq->findClip(id);
        if (!r) continue;
        QJsonObject o = seq->tracks(r->type)[r->track].clips[r->index].toJson();
        o["_type"] = r->type == TrackType::Video ? "v" : "a";
        o["_track"] = r->track;
        o["_offset"] = QString::number(seq->clip(id)->start - first);
        arr.append(o);
    }
    m_clipboard = QJsonDocument(arr).toJson(QJsonDocument::Compact);
    QApplication::clipboard()->setText(QString::fromUtf8(m_clipboard));
    if (cut) deleteSelection(false);
}

void MainWindow::paste(bool insert)
{
    Session *s = S();
    if (m_clipboard.isEmpty()) return;
    const QJsonArray arr = QJsonDocument::fromJson(m_clipboard).array();
    // Map the lowest copied track onto the lowest targeted track (Premiere behaviour).
    int minV = INT32_MAX, minA = INT32_MAX, tv = -1, ta = -1;
    for (const auto &v : arr) {
        const QJsonObject o = v.toObject();
        if (o["_type"] == "v") minV = std::min(minV, o["_track"].toInt()); else minA = std::min(minA, o["_track"].toInt());
    }
    for (const auto &r : s->targetedTracks()) {
        if (r.type == TrackType::Video && tv < 0) tv = r.index;
        if (r.type == TrackType::Audio && ta < 0) ta = r.index;
    }
    if (tv < 0) tv = minV == INT32_MAX ? 0 : minV;
    if (ta < 0) ta = minA == INT32_MAX ? 0 : minA;
    QSet<QString> pasted;
    s->doc().editSequence(insert ? tr("Paste Insert") : tr("Paste"), [&](Sequence &seq) {
        std::vector<std::pair<edit::TrackRef, Clip>> clips;
        QHash<QString, QString> linkMap, groupMap;
        Ticks span = 0;
        for (const auto &v : arr) {
            const QJsonObject o = v.toObject();
            Clip c = Clip::fromJson(o);
            c.id = newId();
            if (!c.linkId.isEmpty()) { if (!linkMap.contains(c.linkId)) linkMap[c.linkId] = newId(); c.linkId = linkMap[c.linkId]; }
            if (!c.groupId.isEmpty()) { if (!groupMap.contains(c.groupId)) groupMap[c.groupId] = newId(); c.groupId = groupMap[c.groupId]; }
            for (auto &e : c.effects) e.uid = newId();
            c.start = seq.playhead + o["_offset"].toString().toLongLong();
            const bool video = o["_type"] == "v";
            int track = video ? tv + (o["_track"].toInt() - minV) : ta + (o["_track"].toInt() - minA);
            auto &tracks = seq.tracks(video ? TrackType::Video : TrackType::Audio);
            while ((int)tracks.size() <= track) seq.addTrack(video ? TrackType::Video : TrackType::Audio);
            span = std::max(span, c.end() - seq.playhead);
            pasted.insert(c.id);
            clips.push_back({{video ? TrackType::Video : TrackType::Audio, track}, c});
        }
        seq.renameTracks();
        const bool ok = insert ? edit::insertEdit(seq, clips, seq.playhead, span) : edit::overwriteEdit(seq, clips);
        if (ok) seq.playhead += span;
        return ok;
    });
    s->select(pasted, false);
}

void MainWindow::pasteAttributes()
{
    Session *s = S();
    const QJsonArray arr = QJsonDocument::fromJson(m_clipboard).array();
    if (arr.isEmpty() || s->selection().isEmpty()) return;
    const Clip src = Clip::fromJson(arr.first().toObject());
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Paste Attributes"));
    auto *v = new QVBoxLayout(&dlg);
    auto *motion = new QCheckBox(tr("Motion"), &dlg), *opacity = new QCheckBox(tr("Opacity"), &dlg), *volume = new QCheckBox(tr("Volume"), &dlg),
         *fx = new QCheckBox(tr("Effects"), &dlg), *speed = new QCheckBox(tr("Time Remapping / Speed"), &dlg);
    for (auto *c : {motion, opacity, volume, fx}) { c->setChecked(true); v->addWidget(c); }
    v->addWidget(speed);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    v->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    const QSet<QString> sel = s->selection();
    s->doc().editSequence(tr("Paste Attributes"), [&](Sequence &seq) {
        for (const auto &id : sel) {
            Clip *c = seq.clip(id);
            if (!c) continue;
            for (const auto &e : src.effects) {
                const EffectDef *d = EffectRegistry::instance().find(e.type);
                if (!d) continue;
                const bool intrinsic = d->intrinsic;
                if (intrinsic) {
                    if ((e.type == "motion" && motion->isChecked()) || (e.type == "opacity" && opacity->isChecked()) ||
                        ((e.type == "volume" || e.type == "panner") && volume->isChecked())) {
                        if (EffectInstance *dst = c->effect(e.type)) { const QString uid = dst->uid; *dst = e; dst->uid = uid; }
                    }
                } else if (fx->isChecked()) {
                    auto r = seq.findClip(id);
                    if ((d->kind == EffectKind::Video) == (r && r->type == TrackType::Video)) {
                        EffectInstance copy = e;
                        copy.uid = newId();
                        c->effects.push_back(copy);
                    }
                }
            }
            if (speed->isChecked()) edit::setSpeed(seq, id, src.speed, src.reverse, false, s->sourceLength());
        }
        return true;
    });
}

void MainWindow::deleteSelection(bool ripple)
{
    Session *s = S();
    if (!s->selectedTransition().isEmpty()) {
        const QString id = s->selectedTransition();
        s->doc().editSequence(tr("Delete Transition"), [&](Sequence &seq) {
            for (auto *v : {&seq.videoTracks, &seq.audioTracks})
                for (auto &t : *v)
                    t.transitions.erase(std::remove_if(t.transitions.begin(), t.transitions.end(), [&](const Transition &x) { return x.id == id; }), t.transitions.end());
            return true;
        });
        return;
    }
    std::vector<QString> ids(s->selection().begin(), s->selection().end());
    if (ids.empty()) {
        if (ripple) s->doc().editSequence(tr("Ripple Delete"), [&](Sequence &seq) {
            bool any = false;
            for (const auto &r : s->targetedTracks()) any = edit::closeGap(seq, r, seq.playhead) || any;
            return any;
        });
        return;
    }
    s->doc().editSequence(ripple ? tr("Ripple Delete") : tr("Clear"), [&](Sequence &seq) {
        if (ripple) edit::rippleDeleteClips(seq, ids); else edit::deleteClips(seq, ids);
        return true;
    });
    s->clearSelection();
}

void MainWindow::liftExtract(bool extract)
{
    Session *s = S();
    const Sequence *seq = s->sequence();
    if (!seq || (seq->inPoint == kInvalidTicks && seq->outPoint == kInvalidTicks)) {
        statusBar()->showMessage(tr("Set In and Out points first"), 3000);
        return;
    }
    const TimeRange r = seq->workRange();
    const auto tracks = s->targetedTracks();
    s->doc().editSequence(extract ? tr("Extract") : tr("Lift"), [&](Sequence &q) {
        if (extract) edit::extract(q, r, tracks); else edit::lift(q, r, tracks);
        q.playhead = r.start;
        q.inPoint = q.outPoint = kInvalidTicks;
        return true;
    });
}

void MainWindow::addEdit(bool allTracks)
{
    Session *s = S();
    s->doc().editSequence(tr("Add Edit"), [&](Sequence &seq) {
        if (!allTracks && !s->selection().isEmpty()) {
            std::vector<QString> ids;
            for (const auto &id : s->selection()) if (const Clip *c = seq.clip(id); c && c->start < seq.playhead && c->end() > seq.playhead) ids.push_back(id);
            if (!ids.empty()) return edit::razorClips(seq, ids, seq.playhead) > 0;
        }
        return edit::razor(seq, seq.playhead, allTracks ? std::vector<edit::TrackRef>{} : s->targetedTracks()) > 0;
    });
}

void MainWindow::applyDefaultTransition(bool video, bool audio, bool toSelection)
{
    Session *s = S();
    const ProjectSettings &ps = s->project().settings;
    const QSet<QString> sel = s->selection();
    s->doc().editSequence(tr("Apply Transition"), [&](Sequence &seq) {
        bool any = false;
        for (TrackType type : {TrackType::Video, TrackType::Audio}) {
            if ((type == TrackType::Video && !video) || (type == TrackType::Audio && !audio)) continue;
            const QString effect = type == TrackType::Video ? ps.defaultVideoTransitionType : ps.defaultAudioTransitionType;
            const Ticks dur = floorToFrame(type == TrackType::Video ? ps.defaultVideoTransition : ps.defaultAudioTransition, seq.fps);
            auto &tracks = seq.tracks(type);
            for (int ti = 0; ti < (int)tracks.size(); ++ti) {
                const Track t = tracks[ti];
                if (t.locked) continue;
                if (toSelection) {
                    // Every cut between two selected clips, plus the open ends.
                    for (const auto &c : t.clips) {
                        if (!sel.contains(c.id)) continue;
                        QString prev, next;
                        for (const auto &o : t.clips) { if (o.end() == c.start) prev = o.id; if (o.start == c.end()) next = o.id; }
                        if (prev.isEmpty() || !sel.contains(prev)) any = !edit::addTransition(seq, {type, ti}, prev, c.id, effect, dur).isEmpty() || any;
                        any = !edit::addTransition(seq, {type, ti}, c.id, next.isEmpty() || !sel.contains(next) ? next : next, effect, dur).isEmpty() || any;
                    }
                } else {
                    if (!t.targeted) continue;
                    // Edit point nearest the playhead.
                    Ticks best = INT64_MAX;
                    QString a, b;
                    for (const auto &c : t.clips)
                        for (Ticks e : {c.start, c.end()})
                            if (std::llabs(e - seq.playhead) < best) {
                                best = std::llabs(e - seq.playhead);
                                a.clear(); b.clear();
                                for (const auto &o : t.clips) { if (o.end() == e) a = o.id; if (o.start == e) b = o.id; }
                            }
                    if (!a.isEmpty() || !b.isEmpty()) any = !edit::addTransition(seq, {type, ti}, a, b, effect, dur).isEmpty() || any;
                }
            }
        }
        return any;
    });
}

void MainWindow::speedDialog()
{
    Session *s = S();
    const Sequence *seq = s->sequence();
    if (!seq || s->selection().isEmpty()) return;
    const Clip *c = seq->clip(s->primaryClip().isEmpty() ? *s->selection().begin() : s->primaryClip());
    if (!c) return;
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Clip Speed / Duration"));
    auto *f = new QFormLayout(&dlg);
    auto *speed = new QDoubleSpinBox(&dlg);
    speed->setRange(1, 100000);
    speed->setDecimals(2);
    speed->setSuffix(" %");
    speed->setValue(c->speed * 100);
    auto *dur = new QLineEdit(formatTimecode(c->duration, seq->fps), &dlg);
    auto *reverse = new QCheckBox(tr("Reverse Speed"), &dlg);
    reverse->setChecked(c->reverse);
    auto *pitch = new QCheckBox(tr("Maintain Audio Pitch"), &dlg);
    pitch->setChecked(c->maintainPitch);
    auto *ripple = new QCheckBox(tr("Ripple Edit, Shifting Trailing Clips"), &dlg);
    const Ticks srcDur = c->sourceDuration();
    connect(speed, QOverload<double>::of(&QDoubleSpinBox::valueChanged), &dlg, [&](double v) {
        QSignalBlocker b(dur);
        dur->setText(formatTimecode(Ticks(double(srcDur) / (v / 100.0)), seq->fps));
    });
    connect(dur, &QLineEdit::editingFinished, &dlg, [&] {
        Ticks d = parseTimecode(dur->text(), seq->fps);
        if (d > 0) { QSignalBlocker b(speed); speed->setValue(double(srcDur) / double(d) * 100.0); }
    });
    f->addRow(tr("Speed"), speed);
    f->addRow(tr("Duration"), dur);
    f->addRow(reverse);
    f->addRow(pitch);
    f->addRow(ripple);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    const QSet<QString> sel = s->selection();
    const double sp = speed->value() / 100.0;
    s->doc().editSequence(tr("Speed/Duration"), [&](Sequence &q) {
        bool any = false;
        for (const auto &id : sel) {
            any = edit::setSpeed(q, id, sp, reverse->isChecked(), ripple->isChecked(), s->sourceLength()) || any;
            if (Clip *cc = q.clip(id)) cc->maintainPitch = pitch->isChecked();
        }
        return any;
    });
}

void MainWindow::audioGainDialog()
{
    Session *s = S();
    if (s->selection().isEmpty()) return;
    bool ok = false;
    double db = QInputDialog::getDouble(this, tr("Audio Gain"), tr("Adjust gain by (dB):"), 0, -96, 96, 1, &ok);
    if (!ok) return;
    const QSet<QString> sel = s->selection();
    s->doc().editSequence(tr("Audio Gain"), [&](Sequence &q) {
        bool any = false;
        for (const auto &id : sel) {
            auto r = q.findClip(id);
            if (!r || r->type != TrackType::Audio) continue;
            Clip *c = q.clip(id);
            AnimatedParam &p = c->effect("volume")->param("level");
            if (p.animated()) for (auto &k : p.keys()) k.value.v[0] = std::clamp(k.value.f() + db, -96.0, 15.0);
            else p.setStaticValue(Value(std::clamp(p.staticValue().f() + db, -96.0, 15.0)));
            any = true;
        }
        return any;
    });
}

void MainWindow::nestSelection()
{
    Session *s = S();
    std::vector<QString> ids(s->selection().begin(), s->selection().end());
    if (ids.empty()) return;
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Nested Sequence Name"), tr("Name:"), QLineEdit::Normal, tr("Nested Sequence %1").arg(s->project().sequences.size()), &ok);
    if (!ok) return;
    s->doc().edit(tr("Nest"), [&](Project &p) {
        Sequence *seq = p.activeSequence();
        return seq && !edit::nest(p, *seq, ids, name).isEmpty();
    });
}

void MainWindow::addMarker()
{
    Session *s = S();
    if (sourceFocused()) {
        const QString id = s->sourceMedia();
        const Ticks t = s->sourcePosition();
        s->doc().edit(tr("Add Marker"), [&](Project &p) {
            MediaItem *m = p.mediaItem(id);
            if (!m) return false;
            Marker mk;
            mk.id = newId();
            mk.time = t;
            m->markers.push_back(mk);
            return true;
        });
        return;
    }
    s->doc().editSequence(tr("Add Marker"), [](Sequence &q) {
        for (auto &m : q.markers)
            if (m.time == q.playhead) return false;
        Marker mk;
        mk.id = newId();
        mk.time = q.playhead;
        mk.name = tr("Marker %1").arg(q.markers.size() + 1);
        q.markers.push_back(mk);
        std::sort(q.markers.begin(), q.markers.end(), [](const Marker &a, const Marker &b) { return a.time < b.time; });
        return true;
    });
}

void MainWindow::matchFrame()
{
    Session *s = S();
    const Sequence *seq = s->sequence();
    if (!seq) return;
    for (int i = (int)seq->videoTracks.size() - 1; i >= 0; --i) {
        const Track &t = seq->videoTracks[i];
        if (!t.targeted || t.muted) continue;
        for (const auto &c : t.clips)
            if (c.start <= seq->playhead && c.end() > seq->playhead && c.source == ClipSource::Media) {
                s->setSourceMedia(c.mediaId);
                s->setSourcePosition(c.sourceAt(seq->playhead));
                m_docks["source"]->show();
                m_docks["source"]->raise();
                return;
            }
    }
}

void MainWindow::markClip()
{
    Session *s = S();
    if (sourceFocused()) return;
    const Sequence *seq = s->sequence();
    if (!seq) return;
    // Selected clip, or the clip under the playhead on targeted tracks.
    const Clip *c = nullptr;
    if (!s->selection().isEmpty()) c = seq->clip(*s->selection().begin());
    if (!c)
        for (const auto &r : s->targetedTracks())
            for (const auto &cl : seq->tracks(r.type)[r.index].clips)
                if (cl.start <= seq->playhead && cl.end() > seq->playhead) { c = &cl; break; }
    if (!c) return;
    const Ticks a = c->start, b = c->end();
    s->doc().editSequence(tr("Mark Clip"), [&](Sequence &q) { q.inPoint = a; q.outPoint = b; return true; });
}

void MainWindow::selectAll()
{
    Session *s = S();
    const Sequence *seq = s->sequence();
    if (!seq) return;
    QSet<QString> ids;
    for (const auto *v : {&seq->videoTracks, &seq->audioTracks})
        for (const auto &t : *v)
            if (!t.locked)
                for (const auto &c : t.clips) ids.insert(c.id);
    s->select(ids, false);
}

void MainWindow::stepPlayhead(int frames)
{
    if (sourceFocused()) {
        m_source->stepFrames(frames);
        return;
    }
    m_program->playback()->stop();
    const Sequence *seq = S()->sequence();
    if (!seq) return;
    S()->setPlayhead(std::max<Ticks>(0, seq->playhead + frames * seq->fps.frameTicks()));
}

void MainWindow::goToEdit(bool forward)
{
    const Sequence *seq = S()->sequence();
    if (!seq) return;
    m_program->playback()->stop();
    const Ticks t = edit::nextEdit(*seq, seq->playhead, forward, S()->targetedTracks());
    if (t != kInvalidTicks) S()->setPlayhead(t);
}

void MainWindow::toggleEnable()
{
    Session *s = S();
    const QSet<QString> sel = s->selection();
    s->doc().editSequence(tr("Enable"), [&](Sequence &q) {
        bool any = false;
        for (const auto &id : sel) if (Clip *c = q.clip(id)) { c->enabled = !c->enabled; any = true; }
        return any;
    });
}

void MainWindow::clipMenuAction(const QString &a)
{
    Session *s = S();
    if (a == "cut") copy(true);
    else if (a == "copy") copy(false);
    else if (a == "paste") paste(false);
    else if (a == "pasteAttributes") pasteAttributes();
    else if (a == "delete") deleteSelection(false);
    else if (a == "rippleDelete") deleteSelection(true);
    else if (a == "toggleEnable") toggleEnable();
    else if (a == "speed") speedDialog();
    else if (a == "audioGain") audioGainDialog();
    else if (a == "normalize") normalizeLoudness();
    else if (a == "nest") nestSelection();
    else if (a == "autoReframe") autoReframe();
    else if (a == "sceneDetect") sceneDetect();
    else if (a == "removeSilence") removeSilences();
    else if (a == "transcribe") transcribe();
    else if (a == "stabilize") stabilize();
    else if (a == "unlink" || a == "link" || a == "group" || a == "ungroup") {
        std::vector<QString> ids(s->selection().begin(), s->selection().end());
        s->doc().editSequence(a, [&](Sequence &q) {
            if (a == "unlink") edit::unlink(q, ids);
            else if (a == "link") edit::link(q, ids);
            else if (a == "group") edit::group(q, ids);
            else edit::ungroup(q, ids);
            return true;
        });
    } else if (a == "fit" || a == "fill") {
        const QSet<QString> sel = s->selection();
        s->doc().editSequence(a == "fit" ? tr("Set to Frame Size") : tr("Scale to Fill"), [&](Sequence &q) {
            for (const auto &id : sel)
                if (Clip *c = q.clip(id))
                    if (EffectInstance *m = c->effect("motion")) {
                        m->param("fit").setStaticValue(Value(a == "fit" ? 0.0 : 1.0));
                        m->param("scale").setValue(Value(100.0), 0);
                    }
            return true;
        });
    } else if (a == "rename") {
        const Sequence *seq = s->sequence();
        const Clip *c = seq ? seq->clip(s->primaryClip()) : nullptr;
        if (!c) return;
        const QString id = c->id;
        QString n = QInputDialog::getText(this, tr("Rename Clip"), tr("Name:"), QLineEdit::Normal, c->name);
        if (!n.isEmpty()) s->doc().editSequence(tr("Rename Clip"), [&](Sequence &q) { if (Clip *cc = q.clip(id)) cc->name = n; return true; });
    } else if (a == "reveal") {
        const Sequence *seq = s->sequence();
        if (const Clip *c = seq ? seq->clip(s->primaryClip()) : nullptr) {
            m_docks["project"]->show();
            m_docks["project"]->raise();
            m_project->reveal(c->mediaId);
        }
    } else if (a == "subclip") {
        const MediaItem *m = s->project().mediaItem(s->sourceMedia());
        if (!m || (m->inPoint == kInvalidTicks && m->outPoint == kInvalidTicks)) {
            statusBar()->showMessage(tr("Set In/Out in the Source monitor to make a subclip"), 3000);
            return;
        }
        MediaItem sub = *m;
        sub.id = newId();
        sub.name = m->name + tr(" (subclip)");
        s->doc().edit(tr("Make Subclip"), [&](Project &p) { p.media.push_back(sub); return true; });
    }
}

// ---------------------------------------------------------------------------
// Assistant

void MainWindow::runTask(const QString &title, std::function<QString(std::function<bool(double)>)> work, std::function<void(const QString &)> done)
{
    if (m_taskRunning) {
        QMessageBox::information(this, title, tr("Another analysis is running."));
        return;
    }
    m_taskRunning = true;
    m_taskBar->setValue(0);
    m_taskBar->setFormat(title + " %p%");
    m_taskBar->show();
    auto progress = std::make_shared<std::atomic<int>>(0);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this, progress] { m_taskBar->setValue(progress->load()); });
    timer->start(100);
    auto *cancelBtn = new QToolButton(this);
    cancelBtn->setText(tr("Cancel"));
    statusBar()->addPermanentWidget(cancelBtn);
    connect(cancelBtn, &QToolButton::clicked, this, [cancel] { cancel->store(true); });
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, timer, cancelBtn, done, cancel] {
        timer->deleteLater();
        cancelBtn->deleteLater();
        m_taskBar->hide();
        m_taskRunning = false;
        const QString r = watcher->result();
        watcher->deleteLater();
        if (cancel->load()) statusBar()->showMessage(tr("Cancelled"), 3000);
        else done(r);
    });
    watcher->setFuture(QtConcurrent::run([work, progress, cancel] {
        return work([progress, cancel](double f) {
            progress->store(int(std::clamp(f, 0.0, 1.0) * 1000));
            return !cancel->load();
        });
    }));
}

struct ClipJob {
    QString clipId;
    QString path;
    int stream = 0;
    Ticks in = 0, out = 0, start = 0, duration = 0;
    double speed = 1;
    bool reverse = false;
};

// Clips to analyse: selection, or every clip of the given kind on the first
// targeted unmuted track.
static std::vector<ClipJob> jobsFor(TrackType type, bool fallbackAll)
{
    std::vector<ClipJob> jobs;
    Session *s = S();
    const Sequence *seq = s->sequence();
    if (!seq) return jobs;
    auto add = [&](const Clip &c) {
        if (c.source != ClipSource::Media) return;
        const MediaItem *m = s->project().mediaItem(c.mediaId);
        if (!m || (type == TrackType::Video ? !m->info.hasVideo || m->info.isStill : !m->info.hasAudio)) return;
        ClipJob j;
        j.clipId = c.id;
        j.path = m->path;
        j.stream = c.audioStream;
        j.in = c.in;
        j.out = c.sourceOut();
        j.start = c.start;
        j.duration = c.duration;
        j.speed = c.speed;
        j.reverse = c.reverse;
        jobs.push_back(j);
    };
    for (const auto &id : s->selection()) {
        auto r = seq->findClip(id);
        if (r && r->type == type) add(*seq->clip(id));
    }
    if (jobs.empty() && fallbackAll) {
        for (const auto &t : seq->tracks(type)) {
            if (t.muted || !t.targeted) continue;
            for (const auto &c : t.clips) add(c);
            if (!jobs.empty()) break;
        }
    }
    return jobs;
}

static Ticks toSeq(const ClipJob &j, Ticks src)
{
    const double local = double(j.reverse ? j.out - src : src - j.in) / j.speed;
    return j.start + Ticks(local);
}

void MainWindow::transcribe()
{
    QString detail;
    if (!ai::whisperAvailable(&detail)) {
        if (QMessageBox::question(this, tr("Transcription"),
                                  tr("Transcription uses Whisper (faster-whisper), which is not installed yet.\n"
                                     "Install it now? (about 300 MB, needs an Internet connection; the speech model is downloaded on first use)")) == QMessageBox::Yes) {
            runTask(tr("Installing transcription engine"), [](std::function<bool(double)> pr) {
                QString err;
                pr(0.05);
                return ai::installWhisper({}, &err) ? QString() : err;
            }, [this](const QString &err) {
                if (err.isEmpty()) transcribe();
                else QMessageBox::warning(this, tr("Transcription"), err);
            });
        }
        return;
    }
    auto jobs = jobsFor(TrackType::Audio, true);
    if (jobs.empty()) {
        QMessageBox::information(this, tr("Transcription"), tr("Select audio clips, or target an audio track that has clips."));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Transcribe Sequence"));
    auto *f = new QFormLayout(&dlg);
    auto *lang = new QComboBox(&dlg);
    const std::pair<const char *, QString> langs[] = {{"auto", tr("Auto detect")}, {"fr", "Francais"}, {"en", "English"}, {"es", "Espanol"}, {"de", "Deutsch"},
                                                      {"it", "Italiano"}, {"pt", "Portugues"}, {"nl", "Nederlands"}, {"ja", "Japanese"}, {"zh", "Chinese"}, {"ar", "Arabic"}};
    for (const auto &[c, n] : langs) lang->addItem(n, c);
    lang->setCurrentIndex(1);
    auto *model = new QComboBox(&dlg);
    for (const char *m : {"tiny", "base", "small", "medium", "large-v3"}) model->addItem(m, m);
    model->setCurrentIndex(2);
    auto *chars = new QSpinBox(&dlg);
    chars->setRange(10, 80);
    chars->setValue(42);
    auto *lines = new QSpinBox(&dlg);
    lines->setRange(1, 3);
    lines->setValue(2);
    auto *social = new QCheckBox(tr("Social style (short, word highlight)"), &dlg);
    f->addRow(tr("Language"), lang);
    f->addRow(tr("Model (bigger = more accurate, slower)"), model);
    f->addRow(tr("Max characters per line"), chars);
    f->addRow(tr("Lines"), lines);
    f->addRow(social);
    connect(social, &QCheckBox::toggled, &dlg, [&](bool on) { chars->setValue(on ? 18 : 42); lines->setValue(on ? 1 : 2); });
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    const QString language = lang->currentData().toString(), modelName = model->currentData().toString();
    const int maxChars = chars->value(), maxLines = lines->value();
    const bool soc = social->isChecked();
    auto result = std::make_shared<std::vector<Caption>>();
    runTask(tr("Transcribing"), [jobs, language, modelName, maxChars, maxLines, result](std::function<bool(double)> pr) {
        QString err;
        for (size_t i = 0; i < jobs.size(); ++i) {
            const auto &j = jobs[i];
            auto segs = ai::transcribe(j.path, j.stream, j.in, j.out, modelName, language, [&](double f) { return pr((i + f) / jobs.size()); }, &err);
            if (!err.isEmpty()) return err;
            for (auto &s : segs) {
                s.start = toSeq(j, s.start);
                s.end = toSeq(j, s.end);
                for (auto &w : s.words) { w.start = toSeq(j, w.start); w.end = toSeq(j, w.end); }
            }
            auto caps = ai::segmentsToCaptions(segs, maxChars, maxLines, 6 * kTicksPerSecond);
            result->insert(result->end(), caps.begin(), caps.end());
        }
        return QString();
    }, [this, result, language, soc](const QString &err) {
        if (!err.isEmpty()) { QMessageBox::warning(this, tr("Transcription"), err); return; }
        S()->doc().editSequence(tr("Transcribe"), [&](Sequence &q) {
            CaptionTrack t;
            t.id = newId();
            t.name = tr("Transcription");
            t.language = language == "auto" ? QString() : language;
            if (soc) {
                t.style.wordHighlight = true;
                t.style.uppercase = true;
                t.style.size = 0.06;
                t.style.positionY = 0.7;
                t.style.maxLines = 1;
            }
            t.captions = *result;
            t.sort();
            q.captionTracks.push_back(t);
            return true;
        });
        statusBar()->showMessage(tr("%n caption(s) created", "", int(result->size())), 5000);
    });
}

void MainWindow::removeSilences()
{
    auto jobs = jobsFor(TrackType::Audio, true);
    if (jobs.empty()) {
        QMessageBox::information(this, tr("Remove Silences"), tr("Select audio clips, or target an audio track that has clips."));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Remove Silences"));
    auto *f = new QFormLayout(&dlg);
    auto *thr = new QDoubleSpinBox(&dlg);
    thr->setRange(-80, -10);
    thr->setValue(-42);
    thr->setSuffix(" dB");
    auto *minD = new QDoubleSpinBox(&dlg);
    minD->setRange(0.1, 10);
    minD->setValue(0.6);
    minD->setSuffix(" s");
    auto *pad = new QDoubleSpinBox(&dlg);
    pad->setRange(0, 2);
    pad->setValue(0.12);
    pad->setSuffix(" s");
    pad->setSingleStep(0.02);
    auto *mode = new QComboBox(&dlg);
    mode->addItem(tr("Remove and close gaps (ripple)"), "ripple");
    mode->addItem(tr("Cut only (keep silences as separate clips)"), "cut");
    f->addRow(tr("Silence below"), thr);
    f->addRow(tr("Minimum silence"), minD);
    f->addRow(tr("Keep around speech"), pad);
    f->addRow(tr("Action"), mode);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    const double t = thr->value();
    const Ticks md = secondsToTicks(minD->value()), pd = secondsToTicks(pad->value());
    const bool ripple = mode->currentData() == "ripple";
    auto ranges = std::make_shared<std::vector<TimeRange>>();
    runTask(tr("Detecting silences"), [jobs, t, md, pd, ranges](std::function<bool(double)> pr) {
        QString err;
        for (size_t i = 0; i < jobs.size(); ++i) {
            const auto &j = jobs[i];
            auto r = ai::detectSilence(j.path, j.stream, j.in, j.out, t, md, pd, [&](double f) { return pr((i + f) / jobs.size()); }, &err);
            for (const auto &x : r) {
                Ticks a = toSeq(j, x.start), b = toSeq(j, x.end);
                if (a > b) std::swap(a, b);
                a = std::max(a, j.start);
                b = std::min(b, j.start + j.duration);
                if (b > a) ranges->push_back({a, b});
            }
        }
        return err;
    }, [this, ranges, ripple](const QString &err) {
        if (!err.isEmpty() && ranges->empty()) { QMessageBox::warning(this, tr("Remove Silences"), err); return; }
        if (ranges->empty()) { statusBar()->showMessage(tr("No silence found"), 4000); return; }
        std::sort(ranges->begin(), ranges->end(), [](const TimeRange &a, const TimeRange &b) { return a.start > b.start; });
        Ticks removed = 0;
        S()->doc().editSequence(tr("Remove Silences"), [&](Sequence &q) {
            for (const auto &r : *ranges) {
                const TimeRange fr{floorToFrame(r.start + q.fps.frameTicks() - 1, q.fps), floorToFrame(r.end, q.fps)};
                if (fr.duration() <= 0) continue;
                if (ripple) {
                    // All tracks so sync is kept (video follows the audio cuts).
                    edit::extract(q, fr, {});
                    removed += fr.duration();
                } else {
                    edit::razor(q, fr.start);
                    edit::razor(q, fr.end);
                }
            }
            return true;
        });
        statusBar()->showMessage(ripple ? tr("%n silence(s) removed, %1 s shorter", "", int(ranges->size())).arg(ticksToSeconds(removed), 0, 'f', 1)
                                        : tr("%n silence(s) cut", "", int(ranges->size())), 6000);
    });
}

void MainWindow::sceneDetect()
{
    auto jobs = jobsFor(TrackType::Video, false);
    if (jobs.empty()) {
        QMessageBox::information(this, tr("Scene Edit Detection"), tr("Select the video clips to analyse."));
        return;
    }
    auto cuts = std::make_shared<std::vector<std::pair<QString, Ticks>>>();
    runTask(tr("Detecting scenes"), [jobs, cuts](std::function<bool(double)> pr) {
        QString err;
        for (size_t i = 0; i < jobs.size(); ++i) {
            const auto &j = jobs[i];
            for (Ticks c : ai::detectScenes(j.path, j.in, j.out, 0.5, [&](double f) { return pr((i + f) / jobs.size()); }, &err))
                cuts->push_back({j.clipId, toSeq(j, c)});
        }
        return err;
    }, [this, cuts](const QString &err) {
        if (cuts->empty()) { statusBar()->showMessage(err.isEmpty() ? tr("No scene change found") : err, 4000); return; }
        S()->doc().editSequence(tr("Scene Edit Detection"), [&](Sequence &q) {
            std::sort(cuts->begin(), cuts->end(), [](auto &a, auto &b) { return a.second > b.second; });
            int n = 0;
            for (const auto &[id, t] : *cuts) {
                // The clip may have been split already: razor whatever clip of the link group covers t.
                const Ticks ft = floorToFrame(t, q.fps);
                for (const auto *v : {&q.videoTracks})
                    for (const auto &tr : *v)
                        for (const auto &c : tr.clips)
                            if (c.start < ft && c.end() > ft && (c.id == id || q.clip(id) == nullptr || c.mediaId == q.clip(id)->mediaId)) {
                                n += edit::razorClips(q, q.linkedClips(c.id), ft);
                                goto next;
                            }
            next:;
            }
            return n > 0;
        });
        statusBar()->showMessage(tr("%n cut(s) added", "", int(cuts->size())), 5000);
    });
}

void MainWindow::autoReframe()
{
    auto jobs = jobsFor(TrackType::Video, false);
    if (jobs.empty()) {
        QMessageBox::information(this, tr("Auto Reframe"), tr("Select the video clips to reframe."));
        return;
    }
    auto tracks = std::make_shared<std::vector<std::pair<ClipJob, std::vector<ai::ReframeSample>>>>();
    runTask(tr("Auto reframe"), [jobs, tracks](std::function<bool(double)> pr) {
        QString err;
        for (size_t i = 0; i < jobs.size(); ++i) {
            auto smp = ai::reframeTrack(jobs[i].path, jobs[i].in, jobs[i].out, 0.6, [&](double f) { return pr((i + f) / jobs.size()); }, &err);
            tracks->push_back({jobs[i], smp});
        }
        return err;
    }, [this, tracks](const QString &) {
        Session *s = S();
        s->doc().editSequence(tr("Auto Reframe"), [&](Sequence &q) {
            for (const auto &[j, smp] : *tracks) {
                Clip *c = q.clip(j.clipId);
                const MediaItem *m = s->project().mediaItem(c ? c->mediaId : QString());
                if (!c || !m || smp.empty()) continue;
                const bool rot = m->info.rotation % 180 != 0;
                const double mw = (rot ? m->info.height : m->info.width) * m->info.pixelAspect, mh = rot ? m->info.width : m->info.height;
                // Fill scale: clip covers the frame.
                const double k = std::max(q.width / mw, q.height / mh);
                const double dispW = mw * k, dispH = mh * k;
                EffectInstance *mo = c->effect("motion");
                mo->param("fit").setStaticValue(Value(1.0));
                AnimatedParam &pos = mo->param("position");
                pos.setAnimated(false);
                pos.setAnimated(true, 0);
                pos.keys().clear();
                for (const auto &sm : smp) {
                    double px = 0.5 - (sm.x - 0.5) * dispW / q.width;
                    double py = 0.5 - (sm.y - 0.5) * dispH / q.height;
                    const double mxx = std::max(0.0, (dispW - q.width) / 2.0 / q.width), mxy = std::max(0.0, (dispH - q.height) / 2.0 / q.height);
                    px = std::clamp(px, 0.5 - mxx, 0.5 + mxx);
                    py = std::clamp(py, 0.5 - mxy, 0.5 + mxy);
                    const Ticks local = toSeq(j, sm.time) - j.start;
                    if (local < 0 || local > j.duration) continue;
                    pos.addKey({local, Value(px, py), Interp::EaseInOut});
                }
                if (pos.keys().empty()) pos.setAnimated(false);
            }
            return true;
        });
        statusBar()->showMessage(tr("Reframed %n clip(s)", "", int(tracks->size())), 4000);
    });
}

void MainWindow::stabilize()
{
    auto jobs = jobsFor(TrackType::Video, false);
    if (jobs.empty()) {
        QMessageBox::information(this, tr("Warp Stabilizer"), tr("Select the video clips to stabilise."));
        return;
    }
    const Sequence *seq = S()->sequence();
    const FrameRate fps = seq->fps;
    // Read existing stabiliser settings (smoothness, method).
    std::vector<std::pair<double, bool>> opts;
    for (const auto &j : jobs) {
        const Clip *c = seq->clip(j.clipId);
        const EffectInstance *e = c ? c->effect("stabilizer") : nullptr;
        opts.push_back({e ? e->f("smoothness", 0) : 50.0, e ? e->value("method", 0).i() == 1 : true});
    }
    auto results = std::make_shared<std::vector<std::pair<QString, QString>>>();
    runTask(tr("Analyzing (stabilizer)"), [jobs, opts, fps, results](std::function<bool(double)> pr) {
        QString err;
        for (size_t i = 0; i < jobs.size(); ++i) {
            const auto &j = jobs[i];
            QString a = ai::analyzeStabilization(j.path, j.in, j.speed, j.duration, fps, opts[i].first, opts[i].second, [&](double f) { return pr((i + f) / jobs.size()); }, &err);
            if (!a.isEmpty()) results->push_back({j.clipId, a});
        }
        return err;
    }, [this, results](const QString &err) {
        if (results->empty()) { QMessageBox::warning(this, tr("Warp Stabilizer"), err.isEmpty() ? tr("Analysis failed") : err); return; }
        S()->doc().editSequence(tr("Warp Stabilizer"), [&](Sequence &q) {
            for (const auto &[id, a] : *results) {
                Clip *c = q.clip(id);
                if (!c) continue;
                EffectInstance *e = c->effect("stabilizer");
                if (!e) { c->effects.push_back(EffectInstance::create("stabilizer")); e = &c->effects.back(); }
                e->param("analysis").setStaticValue(Value(a));
            }
            return true;
        });
        statusBar()->showMessage(tr("Stabilized %n clip(s)", "", int(results->size())), 4000);
    });
}

void MainWindow::createProxies()
{
    Session *s = S();
    QStringList ids;
    m_project->selectedMediaIds(ids);
    if (ids.isEmpty())
        for (const auto &m : s->project().media) if (m.info.hasVideo && !m.info.isStill) ids << m.id;
    std::vector<MediaItem> items;
    for (const auto &id : ids)
        if (const MediaItem *m = s->project().mediaItem(id); m && m->info.hasVideo && !m->info.isStill) items.push_back(*m);
    if (items.empty()) return;
    const QString cache = s->doc().cacheDir();
    const int h = s->project().settings.proxyHeight;
    auto done = std::make_shared<std::vector<std::pair<QString, QString>>>();
    runTask(tr("Creating proxies"), [items, cache, h, done](std::function<bool(double)> pr) {
        QString err;
        for (size_t i = 0; i < items.size(); ++i) {
            const QString dst = proxyPathFor(cache, items[i], h);
            if (!QFileInfo::exists(dst) && !makeProxy(items[i], dst, h, [&](double f) { return pr((i + f) / items.size()); }, &err)) continue;
            done->push_back({items[i].id, dst});
        }
        return err;
    }, [this, done](const QString &err) {
        S()->doc().edit(tr("Attach Proxies"), [&](Project &p) {
            for (const auto &[id, path] : *done) if (MediaItem *m = p.mediaItem(id)) m->proxyPath = path;
            return !done->empty();
        });
        if (!err.isEmpty()) QMessageBox::warning(this, tr("Proxies"), err);
        else statusBar()->showMessage(tr("%n proxy file(s) ready", "", int(done->size())), 4000);
    });
}

void MainWindow::createMulticam()
{
    Session *s = S();
    QStringList ids;
    m_project->selectedMediaIds(ids);
    std::vector<MediaItem> items;
    for (const auto &id : ids)
        if (const MediaItem *m = s->project().mediaItem(id); m && (m->info.hasVideo || m->info.hasAudio)) items.push_back(*m);
    if (items.size() < 2) {
        QMessageBox::information(this, tr("Multi-Camera"), tr("Select at least two clips in the Project panel (one per camera)."));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Create Multi-Camera Source Sequence"));
    auto *form = new QFormLayout(&dlg);
    auto *name = new QLineEdit(QFileInfo(items[0].name).completeBaseName() + tr(" Multicam"), &dlg);
    auto *sync = new QComboBox(&dlg);
    sync->addItem(tr("Audio"), "audio");
    sync->addItem(tr("In Points"), "in");
    sync->addItem(tr("Timecode"), "tc");
    sync->addItem(tr("Clip Start"), "start");
    auto *makeEdit = new QCheckBox(tr("Create an editing sequence and show the multi-camera view"), &dlg);
    makeEdit->setChecked(true);
    form->addRow(tr("Name:"), name);
    form->addRow(tr("Synchronize by:"), sync);
    form->addRow(makeEdit);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString mode = sync->currentData().toString();
    const QString seqName = name->text().trimmed().isEmpty() ? tr("Multicam") : name->text().trimmed();
    const bool edit = makeEdit->isChecked();
    auto offsets = std::make_shared<std::vector<Ticks>>(items.size(), 0);
    auto finish = [this, items, offsets, seqName, edit](const QString &err) {
        if (!err.isEmpty()) {
            QMessageBox::warning(this, tr("Multi-Camera"), err);
            return;
        }
        std::vector<edit::MulticamAngle> angles;
        for (size_t i = 0; i < items.size(); ++i) angles.push_back({items[i].id, (*offsets)[i]});
        QString mcId, editId;
        S()->doc().edit(tr("Create Multi-Camera Source Sequence"), [&](Project &p) {
            mcId = edit::createMulticamSequence(p, angles, seqName);
            const Sequence *mcs = p.sequence(mcId);
            if (!mcs) return false;
            if (edit) {
                Sequence e;
                e.id = newId();
                e.name = seqName + tr(" - Edit");
                e.width = mcs->width;
                e.height = mcs->height;
                e.fps = mcs->fps;
                e.binId = mcs->binId;
                e.ensureDefaultTracks();
                const QString link = newId();
                for (TrackType t : {TrackType::Video, TrackType::Audio}) {
                    Clip c;
                    c.id = newId();
                    c.name = seqName;
                    c.source = ClipSource::Sequence;
                    c.mediaId = mcId;
                    c.duration = mcs->duration();
                    c.linkId = link;
                    c.ensureIntrinsics(t);
                    e.tracks(t)[0].clips.push_back(c);
                }
                editId = e.id;
                p.sequences.push_back(std::move(e));
            }
            return true;
        });
        if (!editId.isEmpty()) {
            S()->setActiveSequence(editId);
            m_program->view()->setMulticamView(true);
        }
        statusBar()->showMessage(tr("Multi-camera sequence created"), 4000);
    };
    if (mode == "audio") {
        QStringList paths;
        for (const auto &m : items) paths << m.path;
        runTask(tr("Synchronizing cameras by audio"), [paths, offsets](std::function<bool(double)> pr) {
            QString err;
            *offsets = ai::audioSyncOffsets(paths, 600, pr, &err);
            return err;
        }, finish);
        return;
    }
    for (size_t i = 0; i < items.size(); ++i) {
        const MediaItem &m = items[i];
        if (mode == "in") {
            (*offsets)[i] = m.inPoint == kInvalidTicks ? 0 : -m.inPoint;
        } else if (mode == "tc") {
            const QStringList f = m.info.timecode.split(QRegularExpression("[:;.]"));
            if (f.size() == 4) {
                const FrameRate r = m.fps().num > 0 ? m.fps() : FrameRate{25, 1};
                const double fpsv = double(r.num) / std::max(1, r.den);
                const double sec = f[0].toInt() * 3600.0 + f[1].toInt() * 60.0 + f[2].toInt() + f[3].toInt() / std::max(1.0, std::round(fpsv));
                (*offsets)[i] = secondsToTicks(sec);
            }
        }
    }
    finish({});
}

void MainWindow::multicamCut(int angle)
{
    Session *s = S();
    const Sequence *seq = s->sequence();
    if (!seq) return;
    std::set<QString> mcs;
    for (const auto &q : s->project().sequences) if (q.multicam) mcs.insert(q.id);
    if (mcs.empty()) return;
    const Ticks t = seq->playhead;
    s->doc().editSequence(tr("Switch Camera"), [&](Sequence &q) { return edit::cutMulticam(q, mcs, t, angle); });
}

void MainWindow::multicamClipAction(const QString &what)
{
    Session *s = S();
    std::vector<QString> ids(s->selection().begin(), s->selection().end());
    if (ids.empty())
        if (const Sequence *seq = s->sequence())
            if (const Clip *c = edit::multicamClipAt(s->project(), *seq, seq->playhead)) ids.push_back(c->id);
    if (ids.empty()) return;
    if (what == "flatten") {
        int n = 0;
        s->doc().edit(tr("Flatten Multi-Camera"), [&](Project &p) {
            Sequence *q = p.activeSequence();
            if (!q) return false;
            n = edit::flattenMulticam(p, *q, ids);
            return n > 0;
        });
        statusBar()->showMessage(tr("%n clip(s) flattened", "", n), 4000);
    } else if (what == "audioFollows") {
        s->doc().editSequence(tr("Audio Follows Video"), [&](Sequence &q) {
            bool any = false;
            for (const auto &id : ids)
                for (const auto &lid : q.linkedClips(id))
                    if (Clip *c = q.clip(lid); c && c->source == ClipSource::Sequence) {
                        c->gen["audioFollowsVideo"] = !multicamAudioFollows(*c);
                        any = true;
                    }
            return any;
        });
    }
}

void MainWindow::normalizeLoudness()
{
    Session *s = S();
    const Sequence *seq = s->sequence();
    if (!seq) return;
    std::vector<QString> ids;
    for (const auto &id : s->selection()) if (auto r = seq->findClip(id); r && r->type == TrackType::Audio) ids.push_back(id);
    if (ids.empty()) return;
    bool ok = false;
    const double target = QInputDialog::getDouble(this, tr("Normalize Loudness"), tr("Target loudness (LUFS):"), -16, -40, -5, 1, &ok);
    if (!ok) return;
    auto project = s->snapshot();
    const Sequence base = *seq;
    auto gains = std::make_shared<std::vector<std::pair<QString, double>>>();
    runTask(tr("Measuring loudness"), [project, base, ids, gains](std::function<bool(double)> pr) {
        for (size_t i = 0; i < ids.size(); ++i) {
            // Measure the clip alone, at unity gain.
            Sequence solo = base;
            for (auto &t : solo.videoTracks) t.clips.clear();
            for (auto &t : solo.audioTracks) { t.clips.clear(); t.transitions.clear(); t.effects.clear(); t.volumeDb = 0; t.muted = false; t.solo = false; }
            solo.masterEffects.clear();
            solo.masterVolumeDb = 0;
            const Clip *c = base.clip(ids[i]);
            if (!c) continue;
            Clip copy = *c;
            copy.start = 0;
            if (EffectInstance *v = copy.effect("volume")) v->param("level") = AnimatedParam(Value(0.0));
            solo.audioTracks.at(0).clips.push_back(copy);
            const double lufs = Exporter::measureLoudness(*project, solo, {0, copy.duration});
            gains->push_back({ids[i], lufs});
            if (!pr(double(i + 1) / ids.size())) break;
        }
        return QString();
    }, [this, gains, target](const QString &) {
        S()->doc().editSequence(tr("Normalize Loudness"), [&](Sequence &q) {
            for (const auto &[id, lufs] : *gains) {
                Clip *c = q.clip(id);
                if (!c || lufs <= -70 || lufs == 0) continue;
                c->effect("volume")->param("level") = AnimatedParam(Value(std::clamp(target - lufs, -60.0, 15.0)));
            }
            return true;
        });
        statusBar()->showMessage(tr("Loudness normalized"), 3000);
    });
}

} // namespace rc
