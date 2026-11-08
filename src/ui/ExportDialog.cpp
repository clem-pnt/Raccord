#include "ExportDialog.h"
#include "export/Exporter.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QOffscreenSurface>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QToolButton>
#include <QVBoxLayout>

namespace rc {

static void setCombo(QComboBox *c, const QString &v)
{
    int i = c->findData(v);
    if (i < 0) { c->addItem(v, v); i = c->count() - 1; }
    c->setCurrentIndex(i);
}

ExportDialog::ExportDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Export Settings"));
    resize(640, 720);
    auto *lay = new QVBoxLayout(this);
    auto *top = new QFormLayout;
    m_preset = new QComboBox(this);
    m_preset->addItem(tr("Custom"), QString());
    for (const auto &p : exportPresets())
        m_preset->addItem(p.category + " - " + p.name, p.id);
    top->addRow(tr("Preset"), m_preset);
    auto *pathRow = new QHBoxLayout;
    m_path = new QLineEdit(this);
    auto *br = new QToolButton(this);
    br->setText("...");
    pathRow->addWidget(m_path, 1);
    pathRow->addWidget(br);
    top->addRow(tr("Output"), pathRow);
    m_range = new QComboBox(this);
    m_range->addItem(tr("Sequence In/Out"), "work");
    m_range->addItem(tr("Entire Sequence"), "all");
    top->addRow(tr("Source Range"), m_range);
    lay->addLayout(top);

    auto *vg = new QGroupBox(tr("Video"), this);
    auto *vf = new QFormLayout(vg);
    m_container = new QComboBox(vg);
    for (const char *c : {"mp4", "mov", "mkv", "webm", "mxf", "png", "tiff", "jpg", "wav", "mp3", "flac", "m4a"}) m_container->addItem(c, c);
    vf->addRow(tr("Format"), m_container);
    m_vcodec = new QComboBox(vg);
    const std::pair<const char *, const char *> vcodecs[] = {{"h264", "H.264"}, {"hevc", "HEVC (H.265)"}, {"av1", "AV1 (SVT)"}, {"vp9", "VP9"},
                                                             {"prores", "Apple ProRes"}, {"dnxhr", "DNxHR"}, {"h264_nvenc", "H.264 NVENC"},
                                                             {"hevc_nvenc", "HEVC NVENC"}, {"png", "PNG"}, {"tiff", "TIFF"}, {"mjpeg", "JPEG"}, {"none", tr("None").toUtf8().constData()}};
    for (const auto &[id, name] : vcodecs)
        if (QString(id) == "none" || encoderAvailable(id)) m_vcodec->addItem(name, id);
    vf->addRow(tr("Codec"), m_vcodec);
    m_profile = new QComboBox(vg);
    for (const char *p : {"", "proxy", "lt", "standard", "hq", "4444", "4444xq", "lb", "sq", "hqx", "444"}) m_profile->addItem(*p ? p : "-", p);
    vf->addRow(tr("Profile"), m_profile);
    m_matchSeq = new QCheckBox(tr("Match sequence settings"), vg);
    m_matchSeq->setChecked(true);
    vf->addRow(m_matchSeq);
    auto *size = new QHBoxLayout;
    m_w = new QSpinBox(vg);
    m_h = new QSpinBox(vg);
    m_w->setRange(16, 16384);
    m_h->setRange(16, 16384);
    size->addWidget(m_w);
    size->addWidget(new QLabel("x"));
    size->addWidget(m_h);
    vf->addRow(tr("Frame Size"), size);
    m_fps = new QDoubleSpinBox(vg);
    m_fps->setRange(1, 240);
    m_fps->setDecimals(3);
    vf->addRow(tr("Frame Rate"), m_fps);
    m_rate = new QComboBox(vg);
    m_rate->addItem(tr("Constant Quality (CRF)"), "crf");
    m_rate->addItem(tr("VBR"), "vbr");
    m_rate->addItem(tr("CBR"), "cbr");
    vf->addRow(tr("Bitrate Encoding"), m_rate);
    m_quality = new QSpinBox(vg);
    m_quality->setRange(0, 63);
    vf->addRow(tr("Quality (CRF)"), m_quality);
    m_vbitrate = new QSpinBox(vg);
    m_vbitrate->setRange(100, 1000000);
    m_vbitrate->setSuffix(" kb/s");
    vf->addRow(tr("Target Bitrate"), m_vbitrate);
    m_encPreset = new QComboBox(vg);
    for (const char *p : {"ultrafast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow"}) m_encPreset->addItem(p, p);
    vf->addRow(tr("Encoder Speed"), m_encPreset);
    m_tenBit = new QCheckBox(tr("10-bit"), vg);
    m_alpha = new QCheckBox(tr("Include alpha channel"), vg);
    auto *bits = new QHBoxLayout;
    bits->addWidget(m_tenBit);
    bits->addWidget(m_alpha);
    vf->addRow(bits);
    lay->addWidget(vg);

    auto *ag = new QGroupBox(tr("Audio"), this);
    auto *af = new QFormLayout(ag);
    m_acodec = new QComboBox(ag);
    for (const auto &[id, name] : std::vector<std::pair<QString, QString>>{{"aac", "AAC"}, {"opus", "Opus"}, {"mp3", "MP3"}, {"flac", "FLAC"},
                                                                          {"pcm16", "PCM 16-bit"}, {"pcm24", "PCM 24-bit"}, {"none", tr("None")}})
        m_acodec->addItem(name, id);
    af->addRow(tr("Codec"), m_acodec);
    m_abitrate = new QSpinBox(ag);
    m_abitrate->setRange(32, 1536);
    m_abitrate->setSuffix(" kb/s");
    af->addRow(tr("Bitrate"), m_abitrate);
    m_sampleRate = new QSpinBox(ag);
    m_sampleRate->setRange(0, 192000);
    m_sampleRate->setSpecialValueText(tr("Sequence"));
    af->addRow(tr("Sample Rate"), m_sampleRate);
    m_channels = new QSpinBox(ag);
    m_channels->setRange(1, 8);
    af->addRow(tr("Channels"), m_channels);
    m_loudness = new QDoubleSpinBox(ag);
    m_loudness->setRange(-40, 0);
    m_loudness->setSpecialValueText(tr("Off"));
    m_loudness->setSuffix(" LUFS");
    af->addRow(tr("Loudness Normalization"), m_loudness);
    m_tp = new QDoubleSpinBox(ag);
    m_tp->setRange(-10, 0);
    m_tp->setSuffix(" dBTP");
    af->addRow(tr("True Peak Limit"), m_tp);
    lay->addWidget(ag);

    auto *eg = new QGroupBox(tr("Effects & Captions"), this);
    auto *ef = new QVBoxLayout(eg);
    m_burn = new QCheckBox(tr("Burn captions into video"), eg);
    m_srt = new QCheckBox(tr("Create sidecar subtitle file (.srt)"), eg);
    m_proxies = new QCheckBox(tr("Use proxies (draft render)"), eg);
    ef->addWidget(m_burn);
    ef->addWidget(m_srt);
    ef->addWidget(m_proxies);
    lay->addWidget(eg);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    m_summary->setStyleSheet("color: #aaa;");
    lay->addWidget(m_summary);
    auto *bb = new QDialogButtonBox(this);
    auto *queue = bb->addButton(tr("Queue"), QDialogButtonBox::ActionRole);
    auto *exp = bb->addButton(tr("Export"), QDialogButtonBox::AcceptRole);
    bb->addButton(QDialogButtonBox::Cancel);
    exp->setDefault(true);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(exp, &QPushButton::clicked, this, [this] { m_queue = false; if (!m_path->text().isEmpty()) accept(); });
    connect(queue, &QPushButton::clicked, this, [this] { m_queue = true; if (!m_path->text().isEmpty()) accept(); });
    connect(br, &QToolButton::clicked, this, &ExportDialog::browse);
    connect(m_preset, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ExportDialog::applyPreset);
    for (QComboBox *c : {m_container, m_vcodec, m_rate, m_acodec, m_profile, m_range, m_encPreset})
        connect(c, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ExportDialog::updateSummary);
    for (QSpinBox *sb : {m_w, m_h, m_quality, m_vbitrate, m_abitrate})
        connect(sb, QOverload<int>::of(&QSpinBox::valueChanged), this, &ExportDialog::updateSummary);
    connect(m_matchSeq, &QCheckBox::toggled, this, [this](bool on) { m_w->setEnabled(!on); m_h->setEnabled(!on); m_fps->setEnabled(!on); updateSummary(); });
    connect(m_container, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        // Keep the extension in sync.
        QString p = m_path->text();
        if (p.isEmpty()) return;
        ExportSettings s = settings();
        QFileInfo fi(p);
        m_path->setText(fi.path() + "/" + fi.completeBaseName() + "." + s.extension());
    });

    Session *s = Session::instance();
    const Sequence *seq = s->sequence();
    QSettings st;
    const QString last = st.value("export/preset", "youtube-1080").toString();
    m_preset->setCurrentIndex(std::max(1, m_preset->findData(last)));
    applyPreset(m_preset->currentIndex());
    if (seq) {
        m_w->setValue(seq->width);
        m_h->setValue(seq->height);
        m_fps->setValue(seq->fps.fps());
        const QString dir = st.value("export/dir", QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).toString();
        m_path->setText(dir + "/" + seq->name + "." + settings().extension());
        m_range->setCurrentIndex(seq->inPoint != kInvalidTicks || seq->outPoint != kInvalidTicks ? 0 : 1);
    }
    m_w->setEnabled(false);
    m_h->setEnabled(false);
    m_fps->setEnabled(false);
    updateSummary();
}

void ExportDialog::applyPreset(int index)
{
    const QString id = m_preset->itemData(index).toString();
    const ExportPreset *p = findPreset(id);
    if (!p) return;
    const ExportSettings &s = p->settings;
    setCombo(m_container, s.container);
    setCombo(m_vcodec, s.videoCodec);
    setCombo(m_rate, s.rateControl);
    setCombo(m_acodec, s.audioCodec);
    setCombo(m_profile, s.profile);
    setCombo(m_encPreset, s.encoderPreset);
    m_quality->setValue(s.quality);
    m_vbitrate->setValue(s.videoBitrateKbps);
    m_abitrate->setValue(s.audioBitrateKbps);
    m_sampleRate->setValue(s.sampleRate);
    m_channels->setValue(s.channels);
    m_loudness->setValue(s.loudnessTarget == 0 ? m_loudness->minimum() : s.loudnessTarget);
    m_tp->setValue(s.truePeakDb);
    m_tenBit->setChecked(s.tenBit);
    m_alpha->setChecked(s.alpha);
    m_burn->setChecked(s.burnCaptions);
    m_srt->setChecked(s.sidecarSrt);
    const bool custom = s.width > 0;
    m_matchSeq->setChecked(!custom);
    if (custom) {
        m_w->setValue(s.width);
        m_h->setValue(s.height);
    }
    if (s.fps.valid() && s.fps.num > 0) m_fps->setValue(s.fps.fps());
    QString p2 = m_path->text();
    if (!p2.isEmpty()) {
        QFileInfo fi(p2);
        m_path->setText(fi.path() + "/" + fi.completeBaseName() + "." + settings().extension());
    }
    updateSummary();
}

ExportSettings ExportDialog::settings() const
{
    ExportSettings s;
    if (const ExportPreset *p = findPreset(m_preset->currentData().toString()))
        s = p->settings;
    s.path = m_path->text();
    s.container = m_container->currentData().toString();
    s.videoCodec = m_vcodec->currentData().toString();
    s.rateControl = m_rate->currentData().toString();
    s.audioCodec = m_acodec->currentData().toString();
    s.profile = m_profile->currentData().toString();
    s.encoderPreset = m_encPreset->currentData().toString();
    s.quality = m_quality->value();
    s.videoBitrateKbps = m_vbitrate->value();
    s.audioBitrateKbps = m_abitrate->value();
    s.sampleRate = m_sampleRate->value();
    s.channels = m_channels->value();
    s.loudnessTarget = m_loudness->value() <= m_loudness->minimum() ? 0 : m_loudness->value();
    s.truePeakDb = m_tp->value();
    s.tenBit = m_tenBit->isChecked();
    s.alpha = m_alpha->isChecked();
    s.burnCaptions = m_burn->isChecked();
    s.sidecarSrt = m_srt->isChecked();
    s.useProxies = m_proxies->isChecked();
    s.useWorkArea = m_range->currentData().toString() == "work";
    if (m_matchSeq->isChecked()) {
        s.width = s.height = 0;
        s.fps = {0, 1};
    } else {
        s.width = m_w->value() & ~1;
        s.height = m_h->value() & ~1;
        s.fps = FrameRate::fromDouble(m_fps->value());
    }
    return s;
}

void ExportDialog::updateSummary()
{
    ExportSettings s = settings();
    const Sequence *seq = Session::instance()->sequence();
    QString txt;
    if (!s.isAudioOnly())
        txt += tr("Video: %1x%2, %3, %4\n").arg(s.width ? s.width : seq ? seq->width : 0).arg(s.height ? s.height : seq ? seq->height : 0)
                   .arg(m_vcodec->currentText()).arg(s.rateControl == "crf" ? tr("CRF %1").arg(s.quality) : tr("%1 kb/s").arg(s.videoBitrateKbps));
    if (s.audioCodec != "none")
        txt += tr("Audio: %1, %2 kb/s%3").arg(m_acodec->currentText()).arg(s.audioBitrateKbps)
                   .arg(s.loudnessTarget != 0 ? tr(", normalized to %1 LUFS").arg(s.loudnessTarget) : QString());
    if (seq) {
        TimeRange r = s.useWorkArea ? seq->workRange() : TimeRange{0, seq->duration()};
        double secs = ticksToSeconds(r.duration());
        if (!s.isAudioOnly() && s.rateControl != "crf")
            txt += tr("\nEstimated size: %1 MB").arg(int(secs * (s.videoBitrateKbps + s.audioBitrateKbps) / 8000.0));
        txt += tr("\nDuration: %1").arg(formatTimecode(r.duration(), seq->fps));
    }
    m_summary->setText(txt);
}

void ExportDialog::browse()
{
    ExportSettings s = settings();
    QString f = QFileDialog::getSaveFileName(this, tr("Export As"), m_path->text(), QString("*.%1").arg(s.extension()));
    if (f.isEmpty()) return;
    if (QFileInfo(f).suffix().isEmpty()) f += "." + s.extension();
    m_path->setText(f);
    QSettings().setValue("export/dir", QFileInfo(f).absolutePath());
}

// ---------------------------------------------------------------------------

RenderWorker::RenderWorker(RenderJob job, QOffscreenSurface *surface, QObject *parent) : QThread(parent), m_job(std::move(job)), m_surface(surface) {}

void RenderWorker::run()
{
    QString err;
    const QString id = m_job.id;
    bool ok = Exporter::run(*m_job.project, m_job.sequenceId, m_job.settings, m_surface, [this, id](const ExportProgress &p) {
        emit progress(id, p.fraction, p.fps, p.etaSeconds, p.preview);
        return !cancel.load();
    }, &err);
    if (cancel.load()) emit finishedJob(id, false, tr("Cancelled"));
    else emit finishedJob(id, ok, ok ? m_job.settings.path : err);
}

RenderQueue::RenderQueue(QWidget *parent) : QWidget(parent)
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    auto *h = new QHBoxLayout;
    m_list = new QListWidget(this);
    h->addWidget(m_list, 1);
    m_preview = new QLabel(this);
    m_preview->setFixedSize(240, 135);
    m_preview->setStyleSheet("background: #111;");
    m_preview->setAlignment(Qt::AlignCenter);
    h->addWidget(m_preview);
    lay->addLayout(h, 1);
    m_bar = new QProgressBar(this);
    m_bar->setRange(0, 1000);
    lay->addWidget(m_bar);
    auto *row = new QHBoxLayout;
    m_status = new QLabel(tr("Queue empty"), this);
    row->addWidget(m_status, 1);
    m_start = new QPushButton(tr("Start Queue"), this);
    m_stop = new QPushButton(tr("Stop"), this);
    auto *remove = new QPushButton(tr("Remove"), this);
    row->addWidget(remove);
    row->addWidget(m_stop);
    row->addWidget(m_start);
    lay->addLayout(row);
    connect(m_start, &QPushButton::clicked, this, [this] { m_running = true; startNext(); });
    connect(m_stop, &QPushButton::clicked, this, [this] {
        m_running = false;
        if (m_worker) m_worker->cancel = true;
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        int r = m_list->currentRow();
        if (r >= 0 && r < (int)m_jobs.size() && m_jobs[r].state != RenderJob::Running) {
            m_jobs.erase(m_jobs.begin() + r);
            refreshList();
        }
    });
}

void RenderQueue::add(const QString &sequenceId, const ExportSettings &s, bool startNow)
{
    Session *ss = Session::instance();
    RenderJob j;
    j.id = newId();
    const Sequence *seq = ss->project().sequence(sequenceId);
    j.name = (seq ? seq->name : QString()) + " -> " + QFileInfo(s.path).fileName();
    j.project = ss->snapshot();
    j.sequenceId = sequenceId;
    j.settings = s;
    m_jobs.push_back(j);
    refreshList();
    if (startNow) {
        m_running = true;
        startNext();
    }
}

void RenderQueue::refreshList()
{
    m_list->clear();
    for (const auto &j : m_jobs) {
        QString st;
        switch (j.state) {
        case RenderJob::Waiting: st = tr("Ready"); break;
        case RenderJob::Running: st = tr("Rendering %1%").arg(int(j.progress * 100)); break;
        case RenderJob::Done: st = tr("Done"); break;
        case RenderJob::Failed: st = tr("Failed: %1").arg(j.message); break;
        case RenderJob::Cancelled: st = tr("Cancelled"); break;
        }
        m_list->addItem(j.name + "   [" + st + "]");
    }
    int waiting = 0;
    for (const auto &j : m_jobs) if (j.state == RenderJob::Waiting) ++waiting;
    if (!m_worker) m_status->setText(waiting ? tr("%n job(s) waiting", "", waiting) : tr("Queue idle"));
}

void RenderQueue::startNext()
{
    if (m_worker || !m_running) return;
    auto it = std::find_if(m_jobs.begin(), m_jobs.end(), [](const RenderJob &j) { return j.state == RenderJob::Waiting; });
    if (it == m_jobs.end()) {
        m_running = false;
        refreshList();
        return;
    }
    if (!m_surface) m_surface = OffscreenGL::createSurface();
    it->state = RenderJob::Running;
    m_worker = new RenderWorker(*it, m_surface, this);
    connect(m_worker, &RenderWorker::progress, this, [this](const QString &id, double f, double fps, double eta, const QImage &preview) {
        for (auto &j : m_jobs) if (j.id == id) j.progress = f;
        m_bar->setValue(int(f * 1000));
        m_status->setText(tr("%1%  -  %2 fps  -  %3 remaining").arg(int(f * 100)).arg(fps, 0, 'f', 1)
                              .arg(QString("%1:%2").arg(int(eta) / 60).arg(int(eta) % 60, 2, 10, QChar('0'))));
        if (!preview.isNull())
            m_preview->setPixmap(QPixmap::fromImage(preview.scaled(m_preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        int row = 0;
        for (auto &j : m_jobs) { if (j.id == id && m_list->item(row)) m_list->item(row)->setText(j.name + "   [" + tr("Rendering %1%").arg(int(f * 100)) + "]"); ++row; }
    });
    connect(m_worker, &RenderWorker::finishedJob, this, [this](const QString &id, bool ok, const QString &msg) {
        QString path;
        for (auto &j : m_jobs)
            if (j.id == id) {
                j.state = ok ? RenderJob::Done : (msg == tr("Cancelled") ? RenderJob::Cancelled : RenderJob::Failed);
                j.message = msg;
                path = j.settings.path;
            }
        m_worker->wait();
        m_worker->deleteLater();
        m_worker = nullptr;
        m_bar->setValue(ok ? 1000 : 0);
        refreshList();
        emit jobFinished(path, ok, msg);
        startNext();
    });
    refreshList();
    m_status->setText(tr("Rendering %1").arg(it->name));
    m_worker->start();
}

} // namespace rc
