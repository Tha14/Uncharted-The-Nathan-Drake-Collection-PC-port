// The window's link to the launcher (uncharted_native.dll's core, launcher/native/core.cpp, run in this process): it
// keeps the window's model (game cards, tabs, status, the shown tab's page) and sends the clicks.
#pragma once

#include <QLibrary>
#include <QObject>
#include <QTimer>
#include <QVariant>

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap chrome READ chrome NOTIFY chromeChanged)
    Q_PROPERTY(QVariantList page READ page NOTIFY pageChanged)
    Q_PROPERTY(QString tab READ tab NOTIFY pageChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(QString error READ error NOTIFY readyChanged)

public:
    // direct: a game to start at once (--play u1 | u2 | u3), the window only shows for a question
    explicit Backend(const QString &direct = {}, QObject *parent = nullptr);
    ~Backend() override;

    QVariantMap chrome() const { return m_chrome; }
    QVariantList page() const { return m_page; }
    QString tab() const { return m_tab; }
    bool ready() const { return m_ready; }
    QString error() const { return m_error; }

    // What a click does: the same Python code as the Tk launcher's widget.
    Q_INVOKABLE void act(const QString &id, const QString &kind, const QVariant &value = {});
    Q_INVOKABLE void selectGame(const QString &key);
    Q_INVOKABLE void playGame(const QString &key);
    Q_INVOKABLE void showTab(const QString &name);
    Q_INVOKABLE void play();
    Q_INVOKABLE void installUpdate();
    Q_INVOKABLE void answer(int dialog, const QVariant &value);
    // a key / mouse button / wheel step for the CONTROLS slot waiting for one (keysym as Tk names it), or Escape
    // for a controller slot
    Q_INVOKABLE void capture(const QString &key, const QString &mouse);
    Q_INVOKABLE void close();
    Q_INVOKABLE QString fileUrl(const QString &path) const;

signals:
    void chromeChanged();
    void pageChanged();
    void readyChanged();
    void messageRequested(int dialog, const QString &kind, const QString &title, const QString &message,
                          const QString &detail);
    void finished();

private:
    void start();
    void send(const QVariantMap &message);
    void handle(const QVariantMap &message);
    void requestModel(bool force = false);
    void fileDialog(int dialog, const QVariantMap &message);
    // pages built natively (uncharted_native.dll un_page / un_page_act, launcher/native/pages.cpp)
    void loadNative(const QString &root);
    bool nativePage(const QString &tab, QVariantList *page);
    bool nativeAct(const QString &id, const QString &kind, const QVariant &value);
    void setUi(const QVariantMap &ui);   // a native page's ui state (CONTROLS: the slot waiting for an input)
    void showCapture();                  // chrome's capturing flags from it
    bool nativeCapturing() const;

    using CoreOpenFn = int (*)(const wchar_t *, const char *);
    using CoreSendFn = int (*)(const char *, char *, int);
    using CoreTickFn = int (*)(char *, int);
    using CoreCloseFn = void (*)();
    QVariantList drain(CoreSendFn send, const char *message);
    QVariantList drain(CoreTickFn tick, std::nullptr_t);
    void deliver(const QVariantList &messages);
    CoreOpenFn m_coreOpen = nullptr;
    CoreSendFn m_coreSend = nullptr;
    CoreTickFn m_coreTick = nullptr;
    CoreCloseFn m_coreClose = nullptr;
    QTimer m_tick;
    QString m_direct;
    bool m_closed = false;
    QTimer m_poll;
    QVariantMap m_chrome;
    QVariantList m_page;
    QVariantMap m_native;   // what native pages are built from, as the Python side sends it with each model:
                            // {config (launcher.json), ini (the game's settings file), gpu, game_dir}
    QString m_root;         // the launcher's folder
    QVariantList m_pythonPage;   // the shown tab's page from the Python side: added after a native page
    QVariantMap m_ui;       // the shown native page's ui state (un_page_act returns it; reset with the tab or game)
    QTimer m_padPoll;       // a CONTROLS controller slot waiting: the controller read every 30 ms
    QByteArray nativeState() const;   // m_native and "root", for un_page / un_page_act
    QLibrary m_dll;
    using PageFn = int (*)(const char *, const char *, char *, int);
    using PageActFn = int (*)(const char *, const char *, const char *, const char *, const char *, char *, int);
    PageFn m_unPage = nullptr;
    PageActFn m_unPageAct = nullptr;
    QString m_tab;
    bool m_ready = false;
    bool m_waiting = false;
    QString m_error;
};
