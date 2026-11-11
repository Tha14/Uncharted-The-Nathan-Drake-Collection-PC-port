// Uncharted Launcher, Qt window. The look is in qml/ (edit in Qt Creator or Qt Design Studio); the launcher itself is
// uncharted_native.dll's core (launcher/native/core.cpp), which Backend runs in this process.
// --play u1 | u2 | u3 (or 1 / 2 / 3): that game starts at once; the window stays hidden unless the launcher asks
// something, and the program ends with the game.
#include <QApplication>
#include <QFont>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStringList>

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

    QString direct;
    const QStringList args = app.arguments();
    const int at = args.indexOf("--play");
    if (at >= 0 && at + 1 < args.size()) {
        const QString v = args.at(at + 1).toLower();
        direct = v.startsWith('u') ? v : "u" + v;
        if (direct != "u1" && direct != "u2" && direct != "u3")
            direct.clear();
    }
    Backend backend(direct);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("backend", &backend);
    QObject::connect(&backend, &Backend::finished, &app, &QCoreApplication::quit);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
                     Qt::QueuedConnection);
    engine.loadFromModule("Launcher", "Main");
    if (!direct.isEmpty() && !engine.rootObjects().isEmpty()) {   // no window unless a question needs one
        QObject *window = engine.rootObjects().first();
        window->setProperty("visible", false);
        QObject::connect(&backend, &Backend::messageRequested, window, [window] { window->setProperty("visible", true); });
    }
    return app.exec();
}
