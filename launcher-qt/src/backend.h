// The bridge to launcher/qt_backend.py: the launcher's Python side runs in that process (its Tk window hidden);
// this object keeps the window's model (game cards, tabs, status, the shown tab's page) and sends the clicks.
#pragma once

#include <QLibrary>
#include <QObject>
#include <QProcess>
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
    explicit Backend(QObject *parent = nullptr);
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
    // a key / mouse button / wheel step for the Controls slot waiting for one (keysym as Tk names it)
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
    void readLines();
    void handle(const QVariantMap &message);
    void requestModel(bool force = false);
    void fileDialog(int dialog, const QVariantMap &message);
    // pages built natively (uncharted_native.dll un_page / un_page_act, launcher/native/pages.cpp)
    void loadNative(const QString &root);
    bool nativePage(const QString &tab, QVariantList *page);
    bool nativeAct(const QString &id, const QString &kind, const QVariant &value);

    QProcess m_process;
    QTimer m_poll;
    QByteArray m_buffer;
    QVariantMap m_chrome;
    QVariantList m_page;
    QVariantMap m_config;   // launcher.json as the Python side holds it (sent with each model)
    QLibrary m_native;
    using PageFn = int (*)(const char *, const char *, char *, int);
    using PageActFn = int (*)(const char *, const char *, const char *, const char *, const char *, char *, int);
    PageFn m_unPage = nullptr;
    PageActFn m_unPageAct = nullptr;
    QString m_tab;
    bool m_ready = false;
    bool m_waiting = false;
    bool m_closing = false;
    QString m_error;
};
