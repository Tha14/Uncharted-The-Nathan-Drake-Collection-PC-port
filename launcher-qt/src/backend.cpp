#include "backend.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

namespace {

// The launcher's folder: the first folder at or above this program's with the launcher's native code.
QString launcherRoot()
{
    const QString override = qEnvironmentVariable("UNCHARTED_LAUNCHER_ROOT");
    if (!override.isEmpty())
        return override;
    QDir dir(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 6; ++i) {
        if (QFileInfo::exists(dir.filePath("launcher/native/uncharted_native.dll")))
            return dir.absolutePath();
        if (!dir.cdUp())
            break;
    }
    return {};
}

} // namespace

Backend::Backend(const QString &direct, QObject *parent) : QObject(parent), m_direct(direct)
{
    m_poll.setInterval(200);
    connect(&m_poll, &QTimer::timeout, this, [this] { requestModel(); });
    m_padPoll.setInterval(30);
    connect(&m_padPoll, &QTimer::timeout, this, [this] {
        nativeAct(QStringLiteral("native:%1:pad_poll").arg(m_tab), QStringLiteral("poll"), QVariantMap{});
    });
    m_tick.setInterval(50);   // the core's timers: the game's output, the imports, the update, the online section
    connect(&m_tick, &QTimer::timeout, this, [this] { deliver(drain(m_coreTick, nullptr)); });
    start();
}

Backend::~Backend()
{
    if (m_coreClose)
        m_coreClose();
}

// The launcher itself is uncharted_native.dll's core (launcher/native/core.cpp), run here: this window sends it the
// player's requests and shows its model, dialogs and events.
void Backend::start()
{
    const QString root = launcherRoot();
    if (root.isEmpty()) {
        m_error = QStringLiteral("launcher/native/uncharted_native.dll was not found next to this program.");
        emit readyChanged();
        return;
    }
    m_root = root;
    loadNative(root);
    m_coreOpen = reinterpret_cast<CoreOpenFn>(m_dll.resolve("un_core_open"));
    m_coreSend = reinterpret_cast<CoreSendFn>(m_dll.resolve("un_core_send"));
    m_coreTick = reinterpret_cast<CoreTickFn>(m_dll.resolve("un_core_tick"));
    m_coreClose = reinterpret_cast<CoreCloseFn>(m_dll.resolve("un_core_close"));
    if (!m_coreOpen || !m_coreSend || !m_coreTick || !m_coreClose) {
        m_error = QStringLiteral("launcher/native/uncharted_native.dll is missing or older than this window.");
        emit readyChanged();
        return;
    }
    QDir(root).mkpath("analysis/uncharted-user");
    QJsonObject args;
    args.insert("direct", m_direct.isEmpty() ? QJsonValue() : QJsonValue(m_direct));
    const QByteArray a = QJsonDocument(args).toJson(QJsonDocument::Compact);
    const std::wstring r = QDir::toNativeSeparators(root).toStdWString();
    m_coreOpen(r.c_str(), a.constData());
    deliver(drain(m_coreSend, nullptr));
    m_tick.start();
}

// what the core says (a JSON array of replies and events), as many calls as its size needs
QVariantList Backend::drain(CoreSendFn send, const char *message)
{
    QByteArray out(1 << 16, '\0');
    int status = send(message, out.data(), int(out.size()));
    while (status < 0) {
        out.resize(-status);
        status = send(nullptr, out.data(), int(out.size()));
    }
    return QJsonDocument::fromJson(QByteArray(out.constData())).array().toVariantList();
}
QVariantList Backend::drain(CoreTickFn tick, std::nullptr_t)
{
    QByteArray out(1 << 16, '\0');
    int status = tick(out.data(), int(out.size()));
    while (status < 0) {
        out.resize(-status);
        status = tick(out.data(), int(out.size()));
    }
    return QJsonDocument::fromJson(QByteArray(out.constData())).array().toVariantList();
}
void Backend::deliver(const QVariantList &messages)
{
    for (const QVariant &m : messages)
        handle(m.toMap());
}

void Backend::send(const QVariantMap &message)
{
    if (!m_coreSend || m_closed)
        return;
    const QByteArray line = QJsonDocument(QJsonObject::fromVariantMap(message)).toJson(QJsonDocument::Compact);
    deliver(drain(m_coreSend, line.constData()));
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
    if (ev == "closed") {   // the launcher ended (closed, or an update's installer takes over)
        m_closed = true;
        m_poll.stop();
        m_tick.stop();
        emit finished();
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
        m_native = data.value("native").toMap();
        showCapture();
        emit chromeChanged();
        const QString tab = data.value("tab").toString();
        QVariantList page = data.value("page").toList();
        m_pythonPage = page;      // a native tab: its page from the DLL, then what the Python side still builds
        nativePage(tab, &page);   // (the Multiplayer tabs' online section)
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
    m_dll.setFileName(QDir(root).filePath("launcher/native/uncharted_native.dll"));
    if (!m_dll.load())
        return;
    m_unPage = reinterpret_cast<PageFn>(m_dll.resolve("un_page"));
    m_unPageAct = reinterpret_cast<PageActFn>(m_dll.resolve("un_page_act"));
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
    QJsonObject state = QJsonObject::fromVariantMap(m_native);
    state.insert("root", QDir::toNativeSeparators(m_root));
    state.insert("ui", QJsonObject::fromVariantMap(m_ui));
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
    *page = QJsonDocument::fromJson(text).array().toVariantList() + m_pythonPage;
    return true;
}

bool Backend::nativeAct(const QString &id, const QString &kind, const QVariant &value)
{
    if (!m_unPageAct || !id.startsWith(QStringLiteral("native:")))
        return false;
    // the page the control is on: its id is "native:<page>:<key>"
    const QByteArray name = id.section(QLatin1Char(':'), 1, 1).toUtf8(), wid = id.toUtf8(), how = kind.toUtf8();
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
    for (const auto &[part, op] : {std::pair{"config", "config"}, std::pair{"ini", "ini"}, std::pair{"game", "game_setting"},
                                     std::pair{"ini_all", "ini_all"}}) {
        const QVariantMap changes = result.value(QLatin1String(part)).toObject().toVariantMap();
        for (auto it = changes.cbegin(); it != changes.cend(); ++it)
            send({{"op", QLatin1String(op)}, {"key", it.key()}, {"value", it.value()}});
    }
    // a launcher action (TOOLS' buttons): the Python side runs it from its fixed list
    if (const QJsonObject call = result.value("call").toObject(); !call.isEmpty()) {
        QVariantMap message{{"op", "call"}, {"name", call.value("name").toString()}};
        if (call.contains("value"))
            message.insert("value", call.value("value").toVariant());
        send(message);
    }
    // the page's new ui state (a slot waiting, the hint): the page again at once, without the Python side
    if (result.contains("ui")) {
        const QVariantMap ui = result.value("ui").toObject().toVariantMap();
        if (ui != m_ui) {
            setUi(ui);
            QVariantList page;
            if (nativePage(m_tab, &page) && page != m_page) {
                m_page = page;
                emit pageChanged();
            }
        }
    }
    return true;
}

void Backend::setUi(const QVariantMap &ui)
{
    m_ui = ui;
    const QVariant pad = m_ui.value("pad_capture");
    if (pad.isValid() && !pad.isNull() && !pad.toString().isEmpty()) {
        if (!m_padPoll.isActive())
            m_padPoll.start();
    } else {
        m_padPoll.stop();
    }
    showCapture();
    emit chromeChanged();
}

bool Backend::nativeCapturing() const
{
    const QVariant keys = m_ui.value("capturing");
    return keys.isValid() && !keys.isNull() && !keys.toList().isEmpty();
}

void Backend::showCapture()
{
    if (nativeCapturing())
        m_chrome.insert("capturing", true);   // the window's capture layer takes the next key or mouse button
    m_chrome.insert("padCapturing", m_padPoll.isActive());   // Escape cancels a controller slot
}

void Backend::selectGame(const QString &key)
{
    setUi({});
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
    setUi({});
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
    if (nativeCapturing() || m_padPoll.isActive()) {   // a native page's slot waits (CONTROLS)
        nativeAct(QStringLiteral("native:%1:capture").arg(m_tab), QStringLiteral("capture"),
                  QVariantMap{{"key", key}, {"mouse", mouse}});
        requestModel(true);
        return;
    }
    send({{"op", "capture"}, {"key", key}, {"mouse", mouse}});
    requestModel(true);
}

void Backend::close()
{
    if (!m_coreSend || m_closed) {
        emit finished();
        return;
    }
    send({{"op", "close"}});
}

QString Backend::fileUrl(const QString &path) const
{
    return path.isEmpty() ? QString() : QUrl::fromLocalFile(path).toString();
}
