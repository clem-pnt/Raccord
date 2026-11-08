#include "ProjectPanel.h"
#include "Icons.h"
#include "Widgets.h"
#include "MediaCache.h"
#include "Timeline.h"
#include "media/MediaProbe.h"
#include "render/Title.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QComboBox>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace rc {

enum ItemRole { IdRole = Qt::UserRole, KindRole };
enum Kind { BinKind = 1, MediaKind, SequenceKind };

QStringList ProjectTree::mimeTypes() const
{
    return {TimelineView::kMediaMime, "text/uri-list"};
}

QMimeData *ProjectTree::mimeData(const QList<QTreeWidgetItem *> &items) const
{
    auto *m = new QMimeData;
    QStringList ids;
    for (auto *it : items)
        if (it->data(0, KindRole).toInt() != BinKind)
            ids << it->data(0, IdRole).toString();
    m->setData(TimelineView::kMediaMime, ids.join('\n').toUtf8());
    return m;
}

void ProjectTree::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls() || e->mimeData()->hasFormat(TimelineView::kMediaMime))
        e->acceptProposedAction();
}

void ProjectTree::dragMoveEvent(QDragMoveEvent *e)
{
    e->acceptProposedAction();
}

void ProjectTree::dropEvent(QDropEvent *e)
{
    QTreeWidgetItem *target = itemAt(e->position().toPoint());
    while (target && target->data(0, KindRole).toInt() != BinKind)
        target = target->parent();
    const QString bin = target ? target->data(0, IdRole).toString() : QString();
    if (e->mimeData()->hasUrls()) {
        QStringList paths;
        for (const auto &u : e->mimeData()->urls())
            if (u.isLocalFile()) paths << u.toLocalFile();
        emit filesDropped(paths, bin);
    } else if (e->mimeData()->hasFormat(TimelineView::kMediaMime)) {
        QStringList ids;
        for (auto *it : selectedItems())
            ids << it->data(0, IdRole).toString();
        emit itemsMoved(ids, bin);
    }
    e->acceptProposedAction();
}

// ---------------------------------------------------------------------------

ProjectPanel::ProjectPanel(QWidget *parent) : QWidget(parent)
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(2);
    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(4, 4, 4, 0);
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search"));
    m_search->setClearButtonEnabled(true);
    bar->addWidget(m_search, 1);
    lay->addLayout(bar);
    m_tree = new ProjectTree(this);
    m_tree->setColumnCount(6);
    m_tree->setHeaderLabels({tr("Name"), tr("Duration"), tr("Frame Rate"), tr("Media Info"), tr("Video Codec"), tr("File Path")});
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setDragEnabled(true);
    m_tree->setAcceptDrops(true);
    m_tree->setDropIndicatorShown(true);
    m_tree->setDragDropMode(QAbstractItemView::DragDrop);
    m_tree->setIconSize(QSize(48, 27));
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_tree->setColumnWidth(0, 220);
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(0, Qt::AscendingOrder);
    lay->addWidget(m_tree, 1);
    auto *bottom = new QHBoxLayout;
    bottom->setContentsMargins(4, 0, 4, 4);
    m_info = new QLabel(this);
    m_info->setStyleSheet("color: #999;");
    bottom->addWidget(m_info, 1);
    auto mk = [&](const QString &icon, const QString &tip, auto fn) {
        auto *b = new QToolButton(this);
        b->setIcon(Icons::get(icon));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        connect(b, &QToolButton::clicked, this, fn);
        bottom->addWidget(b);
        return b;
    };
    mk("import", tr("Import..."), [this] { importDialog(); });
    mk("folder", tr("New Bin"), [this] { newBin(); });
    mk("sequence", tr("New Sequence..."), [this] { newSequence(); });
    mk("trash", tr("Clear"), [this] { deleteSelected(); });
    lay->addLayout(bottom);

    Session *s = Session::instance();
    connect(&s->doc(), &Document::changed, this, &ProjectPanel::rebuild);
    connect(MediaCache::instance(), &MediaCache::updated, this, [this] {
        // Fill in missing thumbnails.
        for (QTreeWidgetItemIterator it(m_tree); *it; ++it)
            if ((*it)->data(0, KindRole).toInt() == MediaKind && (*it)->icon(0).isNull()) {
                const MediaItem *m = Session::instance()->project().mediaItem((*it)->data(0, IdRole).toString());
                if (m && m->info.hasVideo) {
                    QImage img = MediaCache::instance()->thumbnail(m->path, m->info.duration / 3, 96);
                    if (!img.isNull() && img.width() > 1) (*it)->setIcon(0, QIcon(QPixmap::fromImage(img)));
                }
            }
    });
    connect(m_search, &QLineEdit::textChanged, this, &ProjectPanel::rebuild);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) { activate(it); });
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &ProjectPanel::contextMenu);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this, s] {
        auto items = m_tree->selectedItems();
        if (items.size() == 1 && items[0]->data(0, KindRole).toInt() == MediaKind) {
            if (const MediaItem *m = s->project().mediaItem(items[0]->data(0, IdRole).toString()))
                m_info->setText(m->info.summary());
        } else {
            m_info->setText(tr("%n item(s)", "", int(s->project().media.size() + s->project().sequences.size())));
        }
        s->setFocus(Session::Focus::Project);
    });
    connect(m_tree, &ProjectTree::filesDropped, this, [this](const QStringList &p, const QString &bin) { importFiles(p, bin); });
    connect(m_tree, &ProjectTree::itemsMoved, this, [s](const QStringList &ids, const QString &bin) {
        s->doc().edit(tr("Move to Bin"), [&](Project &p) {
            for (const auto &id : ids) {
                if (MediaItem *m = p.mediaItem(id)) m->binId = bin;
                if (Bin *b = p.bin(id); b && b->id != bin) b->parentId = bin;
            }
            return true;
        });
    });
    rebuild();
}

static QString durationText(Ticks d, FrameRate fps)
{
    return formatTimecode(d, fps);
}

void ProjectPanel::rebuild()
{
    if (m_rebuilding)
        return;
    m_rebuilding = true;
    Session *s = Session::instance();
    const Project &p = s->project();
    QSet<QString> selected, collapsed;
    for (auto *it : m_tree->selectedItems())
        selected.insert(it->data(0, IdRole).toString());
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it)
        if ((*it)->data(0, KindRole).toInt() == BinKind && !(*it)->isExpanded())
            collapsed.insert((*it)->data(0, IdRole).toString());
    const int scroll = m_tree->verticalScrollBar() ? 0 : 0;
    (void)scroll;
    m_tree->setSortingEnabled(false);
    m_tree->clear();
    const QString filter = m_search->text().trimmed();
    QHash<QString, QTreeWidgetItem *> bins;
    // Bins (create in passes to honour nesting).
    std::vector<const Bin *> pending;
    for (const auto &b : p.bins) pending.push_back(&b);
    for (int pass = 0; pass < 16 && !pending.empty(); ++pass) {
        std::vector<const Bin *> next;
        for (const Bin *b : pending) {
            QTreeWidgetItem *parent = nullptr;
            if (!b->parentId.isEmpty()) {
                parent = bins.value(b->parentId);
                if (!parent) { next.push_back(b); continue; }
            }
            auto *it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
            it->setText(0, b->name);
            it->setIcon(0, Icons::get("folder"));
            it->setData(0, IdRole, b->id);
            it->setData(0, KindRole, BinKind);
            it->setFlags(it->flags() | Qt::ItemIsDropEnabled);
            bins.insert(b->id, it);
        }
        pending = next;
    }
    auto parentFor = [&](const QString &binId) -> QTreeWidgetItem * { return bins.value(binId); };
    auto add = [&](QTreeWidgetItem *parent) { return parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree); };
    for (const auto &m : p.media) {
        if (!filter.isEmpty() && !m.name.contains(filter, Qt::CaseInsensitive))
            continue;
        auto *it = add(filter.isEmpty() ? parentFor(m.binId) : nullptr);
        it->setText(0, m.name);
        it->setData(0, IdRole, m.id);
        it->setData(0, KindRole, MediaKind);
        it->setFlags(it->flags() & ~Qt::ItemIsDropEnabled);
        if (m.info.hasVideo) {
            QImage img = MediaCache::instance()->thumbnail(m.path, m.info.isStill ? 0 : m.info.duration / 3, 96);
            if (!img.isNull() && img.width() > 1) it->setIcon(0, QIcon(QPixmap::fromImage(img)));
        } else {
            it->setIcon(0, Icons::get("audio"));
        }
        if (!m.info.valid) {
            it->setForeground(0, QColor(230, 90, 90));
            it->setToolTip(0, tr("Media offline: %1").arg(m.info.error));
        }
        it->setText(1, m.info.isStill ? QString() : durationText(m.info.duration, m.fps()));
        it->setData(1, Qt::UserRole + 5, qlonglong(m.info.duration));
        it->setText(2, m.info.hasVideo && !m.info.isStill ? QString::number(m.fps().fps(), 'f', 3).remove(QRegularExpression("\\.?0+$")) + " fps" : QString());
        QString info;
        if (m.info.hasVideo) info = QString("%1 x %2").arg(m.info.width).arg(m.info.height);
        if (m.info.hasAudio) info += (info.isEmpty() ? "" : ", ") + QString("%1 Hz %2ch").arg(m.info.sampleRate).arg(m.info.channels);
        if (!m.proxyPath.isEmpty()) info += tr(" [proxy]");
        it->setText(3, info);
        it->setText(4, m.info.videoCodec);
        it->setText(5, m.path);
        const QColor lc = labelColors()[std::clamp(m.label, 0, (int)labelColors().size() - 1)];
        QPixmap px(4, 16);
        px.fill(lc);
        it->setData(1, Qt::DecorationRole, px);
    }
    for (const auto &sq : p.sequences) {
        if (!filter.isEmpty() && !sq.name.contains(filter, Qt::CaseInsensitive))
            continue;
        auto *it = add(filter.isEmpty() ? parentFor(sq.binId) : nullptr);
        it->setText(0, sq.name);
        it->setIcon(0, Icons::get("sequence"));
        it->setData(0, IdRole, sq.id);
        it->setData(0, KindRole, SequenceKind);
        it->setText(1, durationText(sq.duration(), sq.fps));
        it->setText(2, QString::number(sq.fps.fps(), 'f', 3).remove(QRegularExpression("\\.?0+$")) + " fps");
        it->setText(3, QString("%1 x %2").arg(sq.width).arg(sq.height));
        if (sq.id == p.activeSequenceId) {
            QFont f = it->font(0);
            f.setBold(true);
            it->setFont(0, f);
        }
    }
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        const QString id = (*it)->data(0, IdRole).toString();
        if ((*it)->data(0, KindRole).toInt() == BinKind)
            (*it)->setExpanded(!collapsed.contains(id));
        if (selected.contains(id))
            (*it)->setSelected(true);
    }
    m_tree->setSortingEnabled(true);
    m_info->setText(tr("%n item(s)", "", int(p.media.size() + p.sequences.size())));
    m_rebuilding = false;
}

QString ProjectPanel::currentBin() const
{
    auto *it = m_tree->currentItem();
    while (it && it->data(0, KindRole).toInt() != BinKind)
        it = it->parent();
    return it ? it->data(0, IdRole).toString() : QString();
}

void ProjectPanel::selectedMediaIds(QStringList &out) const
{
    for (auto *it : m_tree->selectedItems())
        if (it->data(0, KindRole).toInt() != BinKind)
            out << it->data(0, IdRole).toString();
}

void ProjectPanel::reveal(const QString &mediaId)
{
    m_search->clear();
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it)
        if ((*it)->data(0, IdRole).toString() == mediaId) {
            m_tree->clearSelection();
            (*it)->setSelected(true);
            m_tree->scrollToItem(*it);
        }
}

void ProjectPanel::importDialog()
{
    QStringList files = QFileDialog::getOpenFileNames(this, tr("Import"), QString(),
                                                      tr("Media (%1);;All files (*)").arg(importFilters().join(' ')));
    if (!files.isEmpty())
        importFiles(files, currentBin());
}

void ProjectPanel::importFiles(const QStringList &pathsIn, const QString &binIn, std::function<void(QStringList)> done)
{
    Session *s = Session::instance();
    // Expand folders into bins.
    struct Job { QString path; QString binId; };
    std::vector<Job> jobs;
    std::vector<Bin> newBins;
    std::function<void(const QString &, const QString &)> addPath = [&](const QString &path, const QString &bin) {
        QFileInfo fi(path);
        if (fi.isDir()) {
            Bin b;
            b.id = newId();
            b.name = fi.fileName();
            b.parentId = bin;
            newBins.push_back(b);
            QDir d(path);
            for (const auto &e : d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
                addPath(e.absoluteFilePath(), b.id);
        } else if (isImportable(path) || path.endsWith(".raccord")) {
            jobs.push_back({fi.absoluteFilePath(), bin});
        }
    };
    for (const auto &p : pathsIn)
        addPath(p, binIn);
    if (jobs.empty()) {
        if (done) done({});
        return;
    }
    if (!newBins.empty())
        s->doc().edit(tr("New Bin"), [&](Project &p) { for (auto &b : newBins) p.bins.push_back(b); return true; });
    emit status(tr("Importing %n file(s)...", "", int(jobs.size())));
    QStringList paths;
    for (const auto &j : jobs) paths << j.path;
    auto *watcher = new QFutureWatcher<MediaInfo>(this);
    connect(watcher, &QFutureWatcher<MediaInfo>::finished, this, [this, watcher, jobs, done, s] {
        QStringList ids;
        QStringList failed;
        s->doc().edit(tr("Import"), [&](Project &p) {
            for (int i = 0; i < (int)jobs.size(); ++i) {
                if (MediaItem *ex = p.mediaByPath(jobs[i].path)) {
                    ids << ex->id;
                    continue;
                }
                MediaInfo info = watcher->resultAt(i);
                if (!info.valid) {
                    failed << QFileInfo(jobs[i].path).fileName() + ": " + info.error;
                    continue;
                }
                MediaItem m;
                m.id = newId();
                m.name = QFileInfo(jobs[i].path).fileName();
                m.path = jobs[i].path;
                m.binId = jobs[i].binId;
                m.info = info;
                p.media.push_back(m);
                ids << m.id;
            }
            return true;
        });
        watcher->deleteLater();
        emit status(tr("Imported %n file(s)", "", int(ids.size())));
        if (!failed.isEmpty())
            QMessageBox::warning(this, tr("Import"), tr("Some files could not be imported:\n%1").arg(failed.join('\n')));
        if (done) done(ids);
    });
    watcher->setFuture(QtConcurrent::mapped(paths, [](const QString &p) { return probeMedia(p); }));
}

void ProjectPanel::newBin()
{
    Session *s = Session::instance();
    const QString parent = currentBin();
    s->doc().edit(tr("New Bin"), [&](Project &p) {
        Bin b;
        b.id = newId();
        b.name = tr("Bin %1").arg(p.bins.size() + 1);
        b.parentId = parent;
        p.bins.push_back(b);
        return true;
    });
}

void ProjectPanel::newSequence()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("New Sequence"));
    auto *f = new QFormLayout(&dlg);
    auto *name = new QLineEdit(tr("Sequence %1").arg(Session::instance()->project().sequences.size() + 1), &dlg);
    auto *preset = new QComboBox(&dlg);
    struct Pre { const char *name; int w, h; int num, den; };
    const Pre presets[] = {
        {"HD 1080p 25", 1920, 1080, 25, 1}, {"HD 1080p 23.976", 1920, 1080, 24000, 1001}, {"HD 1080p 24", 1920, 1080, 24, 1},
        {"HD 1080p 29.97", 1920, 1080, 30000, 1001}, {"HD 1080p 30", 1920, 1080, 30, 1}, {"HD 1080p 50", 1920, 1080, 50, 1},
        {"HD 1080p 59.94", 1920, 1080, 60000, 1001}, {"HD 1080p 60", 1920, 1080, 60, 1}, {"HD 720p 25", 1280, 720, 25, 1},
        {"UHD 4K 25", 3840, 2160, 25, 1}, {"UHD 4K 23.976", 3840, 2160, 24000, 1001}, {"UHD 4K 29.97", 3840, 2160, 30000, 1001},
        {"UHD 4K 50", 3840, 2160, 50, 1}, {"UHD 4K 59.94", 3840, 2160, 60000, 1001}, {"DCI 4K 24", 4096, 2160, 24, 1},
        {"8K UHD 25", 7680, 4320, 25, 1}, {"Vertical 1080x1920 30", 1080, 1920, 30, 1}, {"Vertical 1080x1920 25", 1080, 1920, 25, 1},
        {"Square 1080 30", 1080, 1080, 30, 1}, {"Social 4:5 1080x1350 30", 1080, 1350, 30, 1}, {"PAL SD 720x576 25", 720, 576, 25, 1},
        {"NTSC SD 720x480 29.97", 720, 480, 30000, 1001},
    };
    for (const auto &p : presets) preset->addItem(p.name);
    auto *w = new QSpinBox(&dlg), *h = new QSpinBox(&dlg);
    w->setRange(16, 16384);
    h->setRange(16, 16384);
    w->setValue(1920);
    h->setValue(1080);
    auto *fps = new QComboBox(&dlg);
    const std::pair<const char *, FrameRate> rates[] = {{"23.976", {24000, 1001}}, {"24", {24, 1}}, {"25", {25, 1}}, {"29.97", {30000, 1001}}, {"29.97 DF", {30000, 1001}},
                                                        {"30", {30, 1}}, {"48", {48, 1}}, {"50", {50, 1}}, {"59.94", {60000, 1001}}, {"60", {60, 1}}, {"120", {120, 1}}};
    for (const auto &r : rates) fps->addItem(r.first);
    fps->setCurrentIndex(2);
    auto *v = new QSpinBox(&dlg), *a = new QSpinBox(&dlg);
    v->setRange(1, 99);
    a->setRange(0, 99);
    v->setValue(3);
    a->setValue(3);
    connect(preset, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg, [&](int i) {
        w->setValue(presets[i].w);
        h->setValue(presets[i].h);
        for (int k = 0; k < fps->count(); ++k)
            if (rates[k].second.num == presets[i].num && rates[k].second.den == presets[i].den) { fps->setCurrentIndex(k); break; }
    });
    f->addRow(tr("Name"), name);
    f->addRow(tr("Preset"), preset);
    f->addRow(tr("Width"), w);
    f->addRow(tr("Height"), h);
    f->addRow(tr("Frame rate"), fps);
    f->addRow(tr("Video tracks"), v);
    f->addRow(tr("Audio tracks"), a);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    Session *s = Session::instance();
    QString id = newId();
    const QString bin = currentBin();
    s->doc().edit(tr("New Sequence"), [&](Project &p) {
        Sequence sq;
        sq.id = id;
        sq.name = name->text();
        sq.binId = bin;
        sq.width = w->value() & ~1;
        sq.height = h->value() & ~1;
        sq.fps = rates[fps->currentIndex()].second;
        for (int i = 0; i < v->value(); ++i) sq.addTrack(TrackType::Video);
        for (int i = 0; i < a->value(); ++i) sq.addTrack(TrackType::Audio);
        sq.renameTracks();
        p.sequences.push_back(sq);
        p.activeSequenceId = id;
        return true;
    });
    emit openSequence(id);
}

void ProjectPanel::activate(QTreeWidgetItem *it)
{
    if (!it) return;
    const int kind = it->data(0, KindRole).toInt();
    const QString id = it->data(0, IdRole).toString();
    if (kind == MediaKind) Session::instance()->setSourceMedia(id);
    else if (kind == SequenceKind) emit openSequence(id);
}

void ProjectPanel::deleteSelected()
{
    Session *s = Session::instance();
    QSet<QString> ids;
    for (auto *it : m_tree->selectedItems())
        ids.insert(it->data(0, IdRole).toString());
    if (ids.isEmpty())
        return;
    // Warn if media is used in sequences.
    int used = 0;
    for (const auto &sq : s->project().sequences)
        for (const auto *v : {&sq.videoTracks, &sq.audioTracks})
            for (const auto &t : *v)
                for (const auto &c : t.clips)
                    if (ids.contains(c.mediaId)) ++used;
    if (used > 0 && QMessageBox::question(this, tr("Clear"), tr("%n clip(s) in sequences use these items and will be removed. Continue?", "", used)) != QMessageBox::Yes)
        return;
    s->doc().edit(tr("Clear"), [&](Project &p) {
        // Bins: delete recursively (contents move to root).
        for (auto &m : p.media) if (ids.contains(m.binId)) m.binId.clear();
        for (auto &b : p.bins) if (ids.contains(b.parentId)) b.parentId.clear();
        for (auto &sq : p.sequences) if (ids.contains(sq.binId)) sq.binId.clear();
        p.bins.erase(std::remove_if(p.bins.begin(), p.bins.end(), [&](const Bin &b) { return ids.contains(b.id); }), p.bins.end());
        p.media.erase(std::remove_if(p.media.begin(), p.media.end(), [&](const MediaItem &m) { return ids.contains(m.id); }), p.media.end());
        if (p.sequences.size() > 1 || !ids.contains(p.sequences.front().id))
            p.sequences.erase(std::remove_if(p.sequences.begin(), p.sequences.end(), [&](const Sequence &q) { return ids.contains(q.id) && p.sequences.size() > 1; }), p.sequences.end());
        for (auto &sq : p.sequences)
            for (auto *v : {&sq.videoTracks, &sq.audioTracks})
                for (auto &t : *v)
                    t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(), [&](const Clip &c) { return ids.contains(c.mediaId); }), t.clips.end());
        if (!p.sequence(p.activeSequenceId)) p.activeSequenceId = p.sequences.front().id;
        for (auto &sq : p.sequences) edit::normalize(sq);
        return true;
    });
}

void ProjectPanel::renameSelected()
{
    auto *it = m_tree->currentItem();
    if (!it) return;
    const QString id = it->data(0, IdRole).toString();
    QString n = QInputDialog::getText(this, tr("Rename"), tr("Name:"), QLineEdit::Normal, it->text(0));
    if (n.isEmpty()) return;
    Session::instance()->doc().edit(tr("Rename"), [&](Project &p) {
        if (MediaItem *m = p.mediaItem(id)) m->name = n;
        if (Sequence *s = p.sequence(id)) s->name = n;
        if (Bin *b = p.bin(id)) b->name = n;
        return true;
    });
}

void ProjectPanel::interpretFootage()
{
    auto *it = m_tree->currentItem();
    if (!it || it->data(0, KindRole).toInt() != MediaKind) return;
    const QString id = it->data(0, IdRole).toString();
    const MediaItem *m = Session::instance()->project().mediaItem(id);
    if (!m) return;
    bool ok = false;
    double fps = QInputDialog::getDouble(this, tr("Interpret Footage"), tr("Assume this frame rate (0 = from file):"),
                                         m->fpsOverride ? m->fpsOverride->fps() : 0, 0, 240, 3, &ok);
    if (!ok) return;
    Session::instance()->doc().edit(tr("Interpret Footage"), [&](Project &p) {
        MediaItem *mm = p.mediaItem(id);
        if (!mm) return false;
        if (fps <= 0) mm->fpsOverride.reset();
        else mm->fpsOverride = FrameRate::fromDouble(fps);
        return true;
    });
}

void ProjectPanel::contextMenu(QPoint pos)
{
    Session *s = Session::instance();
    QMenu m(this);
    auto *it = m_tree->itemAt(pos);
    const int kind = it ? it->data(0, KindRole).toInt() : 0;
    if (kind == MediaKind) {
        const QString id = it->data(0, IdRole).toString();
        m.addAction(tr("Open in Source Monitor"), this, [s, id] { s->setSourceMedia(id); });
        m.addAction(tr("New Sequence From Clip"), this, [this, s, id] {
            const MediaItem *mi = s->project().mediaItem(id);
            if (!mi) return;
            const MediaItem copy = *mi;
            QString sid = newId();
            s->doc().edit(tr("New Sequence From Clip"), [&](Project &p) {
                Sequence sq;
                sq.id = sid;
                sq.name = QFileInfo(copy.name).completeBaseName();
                sq.ensureDefaultTracks();
                s->matchSequenceToMedia(sq, copy);
                p.sequences.push_back(sq);
                p.activeSequenceId = sid;
                return true;
            });
            s->doc().editSequence(tr("Overwrite"), [&](Sequence &sq) { return edit::overwriteEdit(sq, s->makeClips(copy, 0, 0, 0)); });
            emit openSequence(sid);
        });
        m.addAction(tr("Interpret Footage..."), this, &ProjectPanel::interpretFootage);
        QMenu *proxy = m.addMenu(tr("Proxy"));
        proxy->addAction(tr("Create Proxies"), this, [this] { emit status("proxy:create"); });
        proxy->addAction(tr("Detach Proxies"), this, [s] {
            s->doc().edit(tr("Detach Proxies"), [&](Project &p) { for (auto &mm : p.media) mm.proxyPath.clear(); return true; });
        });
        m.addAction(tr("Relink Media..."), this, [this, s, id] {
            QString f = QFileDialog::getOpenFileName(this, tr("Relink Media"));
            if (f.isEmpty()) return;
            MediaInfo info = probeMedia(f);
            if (!info.valid) { QMessageBox::warning(this, tr("Relink"), info.error); return; }
            s->doc().edit(tr("Relink Media"), [&](Project &p) {
                MediaItem *mm = p.mediaItem(id);
                if (!mm) return false;
                mm->path = f;
                mm->info = info;
                return true;
            });
        });
        QMenu *labels = m.addMenu(tr("Label"));
        for (int i = 0; i < (int)labelColors().size(); ++i) {
            QPixmap px(12, 12);
            px.fill(labelColors()[i]);
            labels->addAction(QIcon(px), labelColorNames()[i], this, [this, s, i] {
                QStringList ids;
                selectedMediaIds(ids);
                s->doc().edit(tr("Label"), [&](Project &p) { for (const auto &x : ids) if (MediaItem *mm = p.mediaItem(x)) mm->label = i; return true; });
            });
        }
    } else if (kind == SequenceKind) {
        const QString id = it->data(0, IdRole).toString();
        m.addAction(tr("Open in Timeline"), this, [this, id] { emit openSequence(id); });
        m.addAction(tr("Duplicate"), this, [s, id] {
            s->doc().edit(tr("Duplicate Sequence"), [&](Project &p) {
                const Sequence *src = p.sequence(id);
                if (!src) return false;
                Sequence c = *src;
                c.id = newId();
                c.name += tr(" Copy");
                p.sequences.push_back(c);
                return true;
            });
        });
        m.addAction(tr("Sequence Settings..."), this, [this, s, id] {
            const Sequence *sq = s->project().sequence(id);
            if (!sq) return;
            bool ok = false;
            QString size = QInputDialog::getText(this, tr("Sequence Settings"), tr("Frame size (WxH):"), QLineEdit::Normal,
                                                 QString("%1x%2").arg(sq->width).arg(sq->height), &ok);
            if (!ok) return;
            const QStringList wh = size.split('x');
            if (wh.size() != 2) return;
            s->doc().edit(tr("Sequence Settings"), [&](Project &p) {
                Sequence *q = p.sequence(id);
                if (!q) return false;
                q->width = std::max(16, wh[0].toInt()) & ~1;
                q->height = std::max(16, wh[1].toInt()) & ~1;
                return true;
            });
        });
    }
    if (it) {
        m.addAction(tr("Rename..."), this, &ProjectPanel::renameSelected);
        m.addAction(tr("Clear"), this, &ProjectPanel::deleteSelected);
        m.addSeparator();
    }
    m.addAction(tr("Import..."), this, &ProjectPanel::importDialog);
    m.addAction(tr("New Bin"), this, &ProjectPanel::newBin);
    m.addAction(tr("New Sequence..."), this, &ProjectPanel::newSequence);
    m.exec(m_tree->viewport()->mapToGlobal(pos));
}

// ---------------------------------------------------------------------------

class EffectsTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
protected:
    QStringList mimeTypes() const override { return {TimelineView::kEffectMime}; }
    QMimeData *mimeData(const QList<QTreeWidgetItem *> &items) const override
    {
        auto *m = new QMimeData;
        if (!items.isEmpty())
            m->setData(TimelineView::kEffectMime, items.first()->data(0, Qt::UserRole).toString().toUtf8());
        return m;
    }
};

EffectsBrowser::EffectsBrowser(QWidget *parent) : QWidget(parent)
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(2);
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search effects"));
    m_search->setClearButtonEnabled(true);
    lay->addWidget(m_search);
    m_tree = new EffectsTree(this);
    m_tree->setHeaderHidden(true);
    m_tree->setDragEnabled(true);
    m_tree->setDragDropMode(QAbstractItemView::DragOnly);
    lay->addWidget(m_tree, 1);
    connect(m_search, &QLineEdit::textChanged, this, &EffectsBrowser::rebuild);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [](QTreeWidgetItem *it) {
        const QString id = it->data(0, Qt::UserRole).toString();
        if (!id.isEmpty()) applyToSelection(id);
    });
    rebuild();
}

void EffectsBrowser::rebuild()
{
    m_tree->clear();
    const QString f = m_search->text().trimmed();
    struct Root { QString name; EffectKind kind; };
    const Root roots[] = {{tr("Video Effects"), EffectKind::Video}, {tr("Video Transitions"), EffectKind::VideoTransition},
                          {tr("Audio Effects"), EffectKind::Audio}, {tr("Audio Transitions"), EffectKind::AudioTransition}};
    auto &reg = EffectRegistry::instance();
    for (const auto &r : roots) {
        auto *root = new QTreeWidgetItem(m_tree, {r.name});
        root->setIcon(0, Icons::get("folder"));
        root->setFlags(root->flags() & ~Qt::ItemIsDragEnabled);
        for (const QString &cat : reg.categories(r.kind)) {
            auto *ci = new QTreeWidgetItem(root, {trFx(cat)});
            ci->setIcon(0, Icons::get("folder"));
            ci->setFlags(ci->flags() & ~Qt::ItemIsDragEnabled);
            for (const EffectDef *d : reg.all(r.kind)) {
                if (d->category != cat) continue;
                if (!f.isEmpty() && !d->name.contains(f, Qt::CaseInsensitive) && !trFx(d->name).contains(f, Qt::CaseInsensitive)) continue;
                auto *it = new QTreeWidgetItem(ci, {trFx(d->name)});
                it->setData(0, Qt::UserRole, d->id);
                it->setIcon(0, Icons::get("fx"));
                it->setToolTip(0, d->description);
            }
            if (ci->childCount() == 0) delete ci;
        }
        root->setExpanded(!f.isEmpty());
        if (root->childCount() == 0) delete root;
    }
    // Graphics templates and generators.
    auto *g = new QTreeWidgetItem(m_tree, {tr("Graphics & Generators")});
    g->setIcon(0, Icons::get("folder"));
    g->setFlags(g->flags() & ~Qt::ItemIsDragEnabled);
    for (const QString &t : TitleRenderer::templateNames()) {
        if (!f.isEmpty() && !t.contains(f, Qt::CaseInsensitive)) continue;
        auto *it = new QTreeWidgetItem(g, {t});
        it->setData(0, Qt::UserRole, "title:" + t);
        it->setIcon(0, Icons::get("type"));
    }
    const std::pair<QString, QString> gens[] = {{"gen:solid", tr("Color Matte")}, {"gen:adjustment", tr("Adjustment Layer")}, {"gen:bars", tr("Bars and Tone")},
                                                {"gen:countdown", tr("Universal Counting Leader")}, {"gen:transparent", tr("Transparent Video")}};
    for (const auto &[id, name] : gens) {
        if (!f.isEmpty() && !name.contains(f, Qt::CaseInsensitive)) continue;
        auto *it = new QTreeWidgetItem(g, {name});
        it->setData(0, Qt::UserRole, id);
        it->setIcon(0, Icons::get("image"));
    }
    g->setExpanded(!f.isEmpty());
}

void EffectsBrowser::applyToSelection(const QString &id)
{
    Session *s = Session::instance();
    const EffectDef *d = EffectRegistry::instance().find(id);
    if (!d) return;
    const QSet<QString> sel = s->selection();
    if (d->kind == EffectKind::VideoTransition || d->kind == EffectKind::AudioTransition) {
        // Default transition at the edit nearest to the playhead on targeted tracks.
        s->doc().editSequence(tr("Apply Transition"), [&](Sequence &seq) {
            bool any = false;
            const bool video = d->kind == EffectKind::VideoTransition;
            const Ticks ph = seq.playhead;
            const TrackType type = video ? TrackType::Video : TrackType::Audio;
            auto &tracks = seq.tracks(type);
            for (int ti = 0; ti < (int)tracks.size(); ++ti) {
                const Track &t = tracks[ti];
                if (!t.targeted || t.locked) continue;
                QString a, b;
                Ticks best = INT64_MAX;
                for (const auto &c : t.clips) {
                    if (!sel.isEmpty() && !sel.contains(c.id)) continue;
                    for (Ticks edge : {c.start, c.end()}) {
                        Ticks dd = std::llabs(edge - ph);
                        if (dd < best) {
                            best = dd;
                            a.clear(); b.clear();
                            for (const auto &o : t.clips) {
                                if (o.end() == edge) a = o.id;
                                if (o.start == edge) b = o.id;
                            }
                        }
                    }
                }
                if (a.isEmpty() && b.isEmpty()) continue;
                Ticks dur = video ? s->project().settings.defaultVideoTransition : s->project().settings.defaultAudioTransition;
                any = !edit::addTransition(seq, {type, ti}, a, b, id, floorToFrame(dur, seq.fps)).isEmpty() || any;
            }
            return any;
        });
        return;
    }
    s->doc().editSequence(tr("Apply Effect"), [&](Sequence &seq) {
        bool any = false;
        for (const auto &cid : sel) {
            auto r = seq.findClip(cid);
            if (!r) continue;
            if ((d->kind == EffectKind::Video) != (r->type == TrackType::Video)) continue;
            seq.tracks(r->type)[r->track].clips[r->index].effects.push_back(EffectInstance::create(id));
            any = true;
        }
        return any;
    });
}

} // namespace rc
