#include "backend.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QUrl>

namespace {

// The launcher's folder: the first folder at or above this program's with launcher/qt_backend.py.
QString launcherRoot()
{
    const QString override = qEnvironmentVariable("UNCHARTED_LAUNCHER_ROOT");
    if (!override.isEmpty())
        return override;
    QDir dir(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 6; ++i) {
        if (QFileInfo::exists(dir.filePath("launcher/qt_backend.py")))
            return dir.absolutePath();
        if (!dir.cdUp())
            break;
    }
    return {};
}

// The Python that runs the launcher: the package's own (python/), else UNCHARTED_PYTHON, else PATH.
QString pythonFor(const QString &root)
{
    for (const char *name : {"python/pythonw.exe", "python/python.exe"}) {
        const QString path = QDir(root).filePath(QString::fromLatin1(name));
        if (QFileInfo::exists(path))
            return path;
    }
    const QString env = qEnvironmentVariable("UNCHARTED_PYTHON");
    if (!env.isEmpty())
        return env;
    for (const char *name : {"pythonw", "python", "py"}) {
        const QString found = QStandardPaths::findExecutable(QString::fromLatin1(name));
        if (!found.isEmpty())
            return found;
    }
    return {};
}

} // namespace

Backend::Backend(QObject *parent) : QObject(parent)
{
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &Backend::readLines);
    connect(&m_process, &QProcess::finished, this, [this] {
        if (!m_closing && !m_ready) {
            m_error = QStringLiteral("The launcher's Python part stopped at start (see "
                                     "analysis/uncharted-user/launcher-error.log).");
            emit readyChanged();
            return;
        }
        emit finished();
    });
    m_poll.setInterval(200);
    connect(&m_poll, &QTimer::timeout, this, [this] { requestModel(); });
    start();
}

Backend::~Backend()
{
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(2000);
    }
}

void Backend::start()
{
    const QString root = launcherRoot();
    const QString python = pythonFor(root);
    if (root.isEmpty() || python.isEmpty()) {
        m_error = root.isEmpty() ? QStringLiteral("launcher/qt_backend.py was not found next to this program.")
                                 : QStringLiteral("No Python found (the package's python folder is missing).");
        emit readyChanged();
        return;
    }
    m_process.setWorkingDirectory(root);
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    m_process.setStandardErrorFile(QDir(root).filePath("analysis/uncharted-user/qt-backend.log"));
    QDir(root).mkpath("analysis/uncharted-user");
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("PYTHONIOENCODING", "utf-8");
    env.insert("PYTHONUNBUFFERED", "1");
    m_process.setProcessEnvironment(env);
    m_process.start(python, {"-u", QDir(root).filePath("launcher/qt_backend.py")});
}

void Backend::send(const QVariantMap &message)
{
    if (m_process.state() != QProcess::Running)
        return;
    QByteArray line = QJsonDocument(QJsonObject::fromVariantMap(message)).toJson(QJsonDocument::Compact);
    line.append('\n');
    m_process.write(line);
}

void Backend::readLines()
{
    m_buffer += m_process.readAllStandardOutput();
    int at;
    while ((at = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(at).trimmed();
        m_buffer.remove(0, at + 1);
        if (line.isEmpty())
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject())
            handle(doc.object().toVariantMap());
    }
}

void Backend::handle(const QVariantMap &message)
{
    const QString ev = message.value("ev").toString();
    if (ev == "ready") {
        m_ready = true;
        emit readyChanged();
        if (const QString tab = qEnvironmentVariable("UNCHARTED_QT_TAB"); !tab.isEmpty())
            showTab(tab);   // development: open on this tab (screenshots)
        requestModel(true);
        m_poll.start();
        return;
    }
    if (ev == "closed") {
        m_poll.stop();
        return;
    }
    if (ev == "dialog") {
        const int dialog = message.value("dialog").toInt();
        const QString kind = message.value("kind").toString();
        if (kind == "directory" || kind == "open" || kind == "save")
            fileDialog(dialog, message);
        else
            emit messageRequested(dialog, kind, message.value("title").toString(),
                                  message.value("message").toString(), message.value("detail").toString());
        return;
    }
    if (message.value("re").toString() == "model") {
        m_waiting = false;
        if (message.value("same").toBool())
            return;
        const QVariantMap data = message.value("data").toMap();
        m_chrome = data.value("chrome").toMap();
        emit chromeChanged();
        const QVariantList page = data.value("page").toList();
        const QString tab = data.value("tab").toString();
        if (page != m_page || tab != m_tab) {
            m_page = page;
            m_tab = tab;
            emit pageChanged();
        }
    }
}

void Backend::requestModel(bool force)
{
    if (!m_ready || (m_waiting && !force))
        return;
    m_waiting = true;
    send({{"op", "model"}, {"tab", m_tab}, {"force", force}});
}

void Backend::fileDialog(int dialog, const QVariantMap &message)
{
    const QString title = message.value("title").toString();
    const QString dir = message.value("initialdir").toString();
    const QString filter = message.value("filetypes").toString();
    const QString kind = message.value("kind").toString();
    QString path;
    if (kind == "directory")
        path = QFileDialog::getExistingDirectory(nullptr, title, dir);
    else if (kind == "open")
        path = QFileDialog::getOpenFileName(nullptr, title, dir, filter);
    else
        path = QFileDialog::getSaveFileName(nullptr, title,
                                            QDir(dir).filePath(message.value("initialfile").toString()), filter);
    answer(dialog, path);
}

void Backend::act(const QString &id, const QString &kind, const QVariant &value)
{
    send({{"op", "act"}, {"id", id}, {"kind", kind}, {"value", value}});
    requestModel(true);
}

void Backend::selectGame(const QString &key)
{
    send({{"op", "game"}, {"key", key}});
    requestModel(true);
}

void Backend::playGame(const QString &key)
{
    send({{"op", "playgame"}, {"key", key}});
    requestModel(true);
}

void Backend::showTab(const QString &name)
{
    m_tab = name;
    send({{"op", "tab"}, {"name", name}});
    requestModel(true);
}

void Backend::play()
{
    send({{"op", "play"}});
    requestModel(true);
}

void Backend::installUpdate()
{
    send({{"op", "update"}});
}

void Backend::answer(int dialog, const QVariant &value)
{
    send({{"op", "answer"}, {"dialog", dialog}, {"value", value}});
    requestModel(true);
}

void Backend::capture(const QString &key, const QString &mouse)
{
    send({{"op", "capture"}, {"key", key}, {"mouse", mouse}});
    requestModel(true);
}

void Backend::close()
{
    if (m_process.state() != QProcess::Running) {
        emit finished();
        return;
    }
    m_closing = true;
    send({{"op", "close"}});
}

QString Backend::fileUrl(const QString &path) const
{
    return path.isEmpty() ? QString() : QUrl::fromLocalFile(path).toString();
}
