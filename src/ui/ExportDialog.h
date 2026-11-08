#pragma once
// Export Settings dialog and background render queue.
#include "Session.h"
#include "export/ExportSettings.h"
#include <QDialog>
#include <QThread>
#include <QWidget>
#include <atomic>
#include <memory>

class QComboBox;
class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QOffscreenSurface;

namespace rc {

class ExportDialog : public QDialog {
    Q_OBJECT
public:
    explicit ExportDialog(QWidget *parent = nullptr);
    ExportSettings settings() const;
    bool queueOnly() const { return m_queue; }
private:
    void applyPreset(int index);
    void updateSummary();
    void browse();
    QComboBox *m_preset, *m_container, *m_vcodec, *m_rate, *m_acodec, *m_profile, *m_range, *m_encPreset;
    QLineEdit *m_path;
    QSpinBox *m_w, *m_h, *m_quality, *m_vbitrate, *m_abitrate, *m_sampleRate, *m_channels;
    QDoubleSpinBox *m_loudness, *m_tp, *m_fps;
    QCheckBox *m_tenBit, *m_alpha, *m_burn, *m_srt, *m_proxies, *m_matchSeq;
    QLabel *m_summary;
    bool m_queue = false;
};

struct RenderJob {
    QString id;
    QString name;
    std::shared_ptr<const Project> project;
    QString sequenceId;
    ExportSettings settings;
    enum State { Waiting, Running, Done, Failed, Cancelled } state = Waiting;
    double progress = 0;
    QString message;
};

class RenderWorker : public QThread {
    Q_OBJECT
public:
    RenderWorker(RenderJob job, QOffscreenSurface *surface, QObject *parent = nullptr);
    std::atomic<bool> cancel{false};
signals:
    void progress(const QString &id, double fraction, double fps, double eta, const QImage &preview);
    void finishedJob(const QString &id, bool ok, const QString &message);
protected:
    void run() override;
private:
    RenderJob m_job;
    QOffscreenSurface *m_surface;
};

class RenderQueue : public QWidget {
    Q_OBJECT
public:
    explicit RenderQueue(QWidget *parent = nullptr);
    void add(const QString &sequenceId, const ExportSettings &s, bool startNow);
    bool busy() const { return m_worker != nullptr; }
signals:
    void jobFinished(const QString &path, bool ok, const QString &message);
private:
    void startNext();
    void refreshList();
    std::vector<RenderJob> m_jobs;
    RenderWorker *m_worker = nullptr;
    QOffscreenSurface *m_surface = nullptr;
    QListWidget *m_list;
    QProgressBar *m_bar;
    QLabel *m_preview, *m_status;
    QPushButton *m_start, *m_stop;
    bool m_running = false;
};

} // namespace rc
