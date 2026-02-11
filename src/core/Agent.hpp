#pragma once

#include <QCoreApplication>
#include <QDateTime>
#include <QHash>
#include <QLocalServer>
#include <QPointer>
#include <QQueue>
#include <QRegularExpression>
#include <QTimer>
#include <QSharedPointer>
#include <memory>
#include <unordered_map>

#include "Session.hpp"
#include "PolkitListener.hpp"
#include "ipc/IpcServer.hpp"
#include "managers/KeyringManager.hpp"
#include "managers/PinentryManager.hpp"

namespace noctalia {

    class CAgent : public QObject {
        Q_OBJECT

      public:
        explicit CAgent(QObject* parent = nullptr);
        ~CAgent() override;

        // Returns true on success
        bool start(QCoreApplication& app, const QString& socketPath);

      private:
        void onClientDisconnected(QLocalSocket* socket);

        // Message handlers
        void handleMessage(QLocalSocket* socket, const QString& type, const QJsonObject& msg);
        void handleNext(QLocalSocket* socket);
        void handleSubscribe(QLocalSocket* socket);
        void handleKeyringRequest(QLocalSocket* socket, const QJsonObject& msg);
        void handlePinentryRequest(QLocalSocket* socket, const QJsonObject& msg);
        void handlePinentryResult(QLocalSocket* socket, const QJsonObject& msg);
        void handleUIRegister(QLocalSocket* socket, const QJsonObject& msg);
        void handleUIHeartbeat(QLocalSocket* socket, const QJsonObject& msg);
        void handleUIUnregister(QLocalSocket* socket, const QJsonObject& msg);
        void handleRespond(QLocalSocket* socket, const QJsonObject& msg);
        void handleCancel(QLocalSocket* socket, const QJsonObject& msg);

        bool isSessionEventForProviderRouting(const QJsonObject& event) const;
        bool isAuthorizedProviderSocket(QLocalSocket* socket) const;
        bool hasActiveProvider() const;
        bool recomputeActiveProvider(bool emitStatusChange = true);
        void pruneStaleProviders();
        void emitProviderStatus();
        void ensureFallbackUiRunning(const QString& reason);

        // Polkit event handlers (called directly by PolkitListener)
        void onPolkitCompleted(bool gainedAuthorization);

      public:
        void onPolkitRequest(const QString& cookie, const QString& message,
                             const QString& iconName, const QString& actionId,
                             const QString& user, const PolkitQt1::Details& details);
        void onSessionRequest(const QString& cookie, const QString& prompt, bool echo);
        void onSessionComplete(const QString& cookie, bool success);
        void onSessionRetry(const QString& cookie, const QString& error);

        void emitSessionEvent(const QJsonObject& event);

        // Centralized session management - used by all managers
        void createSession(const QString& id, Session::Source source, Session::Context ctx);
        void updateSessionPrompt(const QString& id, const QString& prompt, bool echo = false, bool clearError = true);
        void updateSessionError(const QString& id, const QString& error);
        void updateSessionPinentryRetry(const QString& id, int curRetry, int maxRetries);
        // Returns closed event if deferred=true, otherwise emits immediately and returns empty object
        QJsonObject closeSession(const QString& id, Session::Result result, bool deferred = false);
        Session* getSession(const QString& id);

      private:
        noctalia::IpcServer             m_ipcServer;
        noctalia::KeyringManager        m_keyringManager;
        noctalia::PinentryManager       m_pinentryManager;

        QSharedPointer<CPolkitListener> m_listener;

        // Queue of pending events for UI
        QQueue<QJsonObject> eventQueue;

        // Waiters for "next" event
        QList<QLocalSocket*> nextWaiters;

        // Active sessions
        std::unordered_map<QString, std::unique_ptr<noctalia::Session>> m_sessions;

        // Active subscribers
        QList<QLocalSocket*> m_subscribers;

        struct UIProvider {
            QString id;
            QString name;
            QString kind;
            int     priority = 0;
            qint64  lastHeartbeatMs = 0;
        };

        QHash<QLocalSocket*, UIProvider> m_uiProviders;
        QPointer<QLocalSocket>           m_activeProvider;
        QTimer                           m_providerMaintenanceTimer;
        QString                          m_socketPath;
        qint64                           m_lastFallbackLaunchMs = 0;

        void processNextWaiter();
    };

} // namespace noctalia

using CAgent = noctalia::CAgent;
extern std::unique_ptr<CAgent> g_pAgent;
