// Uncharted Launcher, Qt window. The look is in qml/ (edit in Qt Creator or Qt Design Studio); the launcher's
// logic stays in launcher/uncharted_launcher.pyw, run by launcher/qt_backend.py (Backend).
#include <QApplication>
#include <QFont>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

#include "backend.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);   // QApplication (not QGuiApplication): the native file dialogs
    app.setApplicationName("Uncharted Launcher");
    app.setOrganizationName("Uncharted Launcher");
    app.setWindowIcon(QIcon(":/qt/qml/Launcher/assets/uncharted.ico"));
    QFont font("Segoe UI", 10);
    app.setFont(font);
    QQuickStyle::setStyle("Basic");

    Backend backend;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("backend", &backend);
    QObject::connect(&backend, &Backend::finished, &app, &QCoreApplication::quit);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
                     Qt::QueuedConnection);
    engine.loadFromModule("Launcher", "Main");
    return app.exec();
}
