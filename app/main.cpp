#include "Version.h"
#include "media/FFmpeg.h"
#include "ui/Icons.h"
#include "ui/MainWindow.h"
#include "ofx/OfxHost.h"
#include "ui/Session.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QLibraryInfo>
#include <QLocale>
#include <QSurfaceFormat>
#include <QTranslator>

int main(int argc, char **argv)
{
    // Monitors and offscreen renderers share textures.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(0);
    fmt.setStencilBufferSize(0);
    fmt.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);
    QApplication::setApplicationName("Raccord");
    QApplication::setOrganizationName("Raccord");
    QApplication::setApplicationVersion(RACCORD_VERSION);
    QApplication::setDesktopFileName("raccord");

    Q_INIT_RESOURCE(ui);
    QTranslator qtTr, appTr;
    const QLocale loc;
    if (qtTr.load(loc, "qtbase", "_", QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(&qtTr);
    if (appTr.load(loc, "raccord", "_", ":/i18n"))
        app.installTranslator(&appTr);

    QCommandLineParser parser;
    parser.setApplicationDescription(QObject::tr("Raccord video editor"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("files", QObject::tr("Project (.raccord, .prproj, .xml, .otio, .edl) or media files to open"));
    parser.process(app);

    rc::ff::initLogging();
    rc::ofx::Host::instance().loadPlugins();
    rc::applyTheme(app);
    rc::Session session;
    rc::MainWindow w;
    w.resize(1600, 960);
    w.show();
    const QStringList files = parser.positionalArguments();
    if (!files.isEmpty()) {
        QStringList abs;
        for (const auto &f : files) abs << QDir::current().absoluteFilePath(f);
        w.importPaths(abs);
    }
    return app.exec();
}
