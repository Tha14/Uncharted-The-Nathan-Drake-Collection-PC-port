#include "backend.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
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
    m_root = root;
    loadNative(root);
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
        m_config = data.value("config").toMap();
        m_ini = data.value("ini").toMap();
        m_gpu = data.value("gpu").toMap();
        emit chromeChanged();
        const QString tab = data.value("tab").toString();
        QVariantList page = data.value("page").toList();
        nativePage(tab, &page);   // a native tab: its page from the DLL (the Python side has none)
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
    if (!nativeAct(id, kind, value))
        send({{"op", "act"}, {"id", id}, {"kind", kind}, {"value", value}});
    requestModel(true);
}

void Backend::loadNative(const QString &root)
{
    m_native.setFileName(QDir(root).filePath("launcher/native/uncharted_native.dll"));
    if (!m_native.load())
        return;
    m_unPage = reinterpret_cast<PageFn>(m_native.resolve("un_page"));
    m_unPageAct = reinterpret_cast<PageActFn>(m_native.resolve("un_page_act"));
}

namespace {
// A call that fills a text buffer (the DLL's convention: 0 ok, a negative status is the size it needs).
template <typename Call> int nativeText(Call call, QByteArray *text)
{
    QByteArray out(1 << 16, '\0');
    int status = call(out.data(), int(out.size()));
    if (status < 0) {
        out.resize(-status);
        status = call(out.data(), int(out.size()));
    }
    if (status == 0)
        *text = QByteArray(out.constData());
    return status;
}
} // namespace

QByteArray Backend::nativeState() const
{
    QJsonObject state;
    state.insert("config", QJsonObject::fromVariantMap(m_config));
    state.insert("ini", QJsonObject::fromVariantMap(m_ini));
    state.insert("root", QDir::toNativeSeparators(m_root));
    state.insert("gpu", QJsonObject::fromVariantMap(m_gpu));
    return QJsonDocument(state).toJson(QJsonDocument::Compact);
}

bool Backend::nativePage(const QString &tab, QVariantList *page)
{
    if (!m_unPage || tab.isEmpty())
        return false;
    const QByteArray name = tab.toUtf8();
    const QByteArray config = nativeState();
    QByteArray text;
    if (nativeText([&](char *out, int cap) { return m_unPage(name.constData(), config.constData(), out, cap); }, &text) != 0)
        return false;
    *page = QJsonDocument::fromJson(text).array().toVariantList();
    return true;
}

bool Backend::nativeAct(const QString &id, const QString &kind, const QVariant &value)
{
    if (!m_unPageAct || !id.startsWith(QStringLiteral("native:")))
        return false;
    const QByteArray name = m_tab.toUtf8(), wid = id.toUtf8(), how = kind.toUtf8();
    const QByteArray config = nativeState();
    QJsonArray boxed;
    boxed.append(QJsonValue::fromVariant(value));
    QByteArray json = QJsonDocument(boxed).toJson(QJsonDocument::Compact);
    json = json.mid(1, json.size() - 2);   // the value alone
    QByteArray text;
    if (nativeText([&](char *out, int cap) {
            return m_unPageAct(name.constData(), wid.constData(), how.constData(), json.constData(),
                               config.constData(), out, cap);
        }, &text) != 0)
        return true;   // a native control: nothing for the Python side
    // each change through the Python side's own code: launcher.json (change_config), the game's settings file
    // (change: the preset becomes Custom ...), the game's launcher settings (change_game_setting)
    const QJsonObject result = QJsonDocument::fromJson(text).object();
    for (const auto &[part, op] : {std::pair{"config", "config"}, std::pair{"ini", "ini"}, std::pair{"game", "game_setting"}}) {
        const QVariantMap changes = result.value(QLatin1String(part)).toObject().toVariantMap();
        for (auto it = changes.cbegin(); it != changes.cend(); ++it)
            send({{"op", QLatin1String(op)}, {"key", it.key()}, {"value", it.value()}});
    }
    return true;
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
