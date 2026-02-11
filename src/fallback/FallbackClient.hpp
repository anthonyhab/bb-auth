#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>

namespace noctalia {

class FallbackClient : public QObject {
    Q_OBJECT

  public:
    explicit FallbackClient(const QString& socketPath, QObject* parent = nullptr);

    void start();

    bool isConnected() const;
    bool isActiveProvider() const;

    void sendResponse(const QString& id, const QString& response);
    void sendCancel(const QString& id);

  signals:
    void connectionStateChanged(bool connected);
    void providerStateChanged(bool active);
    void statusMessage(const QString& status);

    void sessionCreated(const QJsonObject& event);
    void sessionUpdated(const QJsonObject& event);
    void sessionClosed(const QJsonObject& event);

  private:
    void ensureConnected();
    void sendJson(const QJsonObject& json);
    void registerProvider();
    void subscribe();
    void setProviderActive(bool active);
    void applyPendingProviderState();
    void handleMessage(const QJsonObject& msg);

    QString      m_socketPath;
    QLocalSocket m_socket;
    QByteArray   m_buffer;

    QTimer       m_reconnectTimer;
    QTimer       m_subscribeWatchdog;
    QTimer       m_heartbeatTimer;

    int          m_reconnectDelayMs = 200;
    bool         m_subscribed = false;
    bool         m_registered = false;
    bool         m_providerActive = false;
    QString      m_providerId;
    bool         m_pendingProviderActiveKnown = false;
    bool         m_pendingProviderActive = false;
    QString      m_pendingProviderId;
};

} // namespace noctalia
