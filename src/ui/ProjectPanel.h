#pragma once
// Project panel (bins, media, sequences) and Effects browser.
#include "Session.h"
#include <QTreeWidget>
#include <QWidget>
#include <functional>

class QLineEdit;
class QLabel;

namespace rc {

class ProjectTree : public QTreeWidget {
    Q_OBJECT
public:
    using QTreeWidget::QTreeWidget;
protected:
    QMimeData *mimeData(const QList<QTreeWidgetItem *> &items) const override;
    QStringList mimeTypes() const override;
    void dropEvent(QDropEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
signals:
    void filesDropped(const QStringList &paths, const QString &binId);
    void itemsMoved(const QStringList &ids, const QString &binId);
};

class ProjectPanel : public QWidget {
    Q_OBJECT
public:
    explicit ProjectPanel(QWidget *parent = nullptr);
    // Probes files in the background and adds them to the project. `done`
    // receives the new (or existing) media ids in order.
    void importFiles(const QStringList &paths, const QString &binId = {}, std::function<void(QStringList)> done = {});
    void importDialog();
    void newSequence();
    void newBin();
    QString currentBin() const;
    void reveal(const QString &mediaId);
    void selectedMediaIds(QStringList &out) const;

signals:
    void openSequence(const QString &id);
    void status(const QString &msg);

private:
    void rebuild();
    void contextMenu(QPoint pos);
    void activate(QTreeWidgetItem *item);
    void deleteSelected();
    void renameSelected();
    void interpretFootage();
    ProjectTree *m_tree;
    QLineEdit *m_search;
    QLabel *m_info;
    bool m_rebuilding = false;
    int m_pendingImports = 0;
};

class EffectsBrowser : public QWidget {
    Q_OBJECT
public:
    explicit EffectsBrowser(QWidget *parent = nullptr);
    // Applies an effect or transition id to the current selection (double-click).
    static void applyToSelection(const QString &id);
private:
    void rebuild();
    QTreeWidget *m_tree;
    QLineEdit *m_search;
};

} // namespace rc
