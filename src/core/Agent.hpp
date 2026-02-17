#pragma once

#include <QCoreApplication>
#include <QSharedPointer>
#include <QTimer>

#include <memory>

#include "PolkitListener.hpp"
#include "Session.hpp"
#include "agent/EventQueue.hpp"
#include "agent/EventRouter.hpp"
#include "agent/ProviderRegistry.hpp"
#include "agent/SessionStore.hpp"
#include "agent/MessageRouter.hpp"
#include "ipc/IpcServer.hpp"
#include "managers/KeyringManager.hpp"
#include "managers/PinentryManager.hpp"

namespace bb {

    class CAgent : public QObject {
        Q_OBJECT

      public:
        explicit CAgent(QObject* parent = nullptr);
        ~CAgent() override;

        bool start(QCoreApplication& app, const QString& socketPath);

      private:
        void onClientDisconnected(QLocalSocket* socket);

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

        bool isAuthorizedProviderSocket(QLocalSocket* socket) const;
        bool hasActiveProvider() const;
        void pruneStaleProviders();
        void emitProviderStatus();
        void ensureFallbackUiRunning(const QString& reason);

        void onPolkitCompleted(bool gainedAuthorization);

      public:
        void        onPolkitRequest(const QString& cookie, const QString& message, const QString& iconName, const QString& actionId, const QString& user,
                                    const PolkitQt1::Details& details);
        void        onSessionRequest(const QString& cookie, const QString& prompt, bool echo);
        void        onSessionComplete(const QString& cookie, bool success);
        void        onSessionRetry(const QString& cookie, const QString& error);
        void        onSessionInfo(const QString& cookie, const QString& info);

        void        emitSessionEvent(const QJsonObject& event);

        void        createSession(const QString& id, Session::Source source, Session::Context ctx);
        void        updateSessionPrompt(const QString& id, const QString& prompt, bool echo = false, bool clearError = true);
        void        updateSessionError(const QString& id, const QString& error);
        void        updateSessionPinentryRetry(const QString& id, int curRetry, int maxRetries);
        QJsonObject closeSession(const QString& id, Session::Result result, bool deferred = false);
        Session*    getSession(const QString& id);

      private:
        bb::IpcServer                   m_ipcServer;
        bb::KeyringManager              m_keyringManager;
        bb::PinentryManager             m_pinentryManager;

        QSharedPointer<CPolkitListener> m_listener;
        bb::agent::ProviderRegistry     m_providerRegistry;
        bb::agent::EventQueue           m_eventQueue;
        bb::agent::EventRouter          m_eventRouter;
        bb::agent::SessionStore         m_sessionStore;
        bb::agent::MessageRouter        m_messageRouter;
        QList<QLocalSocket*>            m_subscribers;
        QTimer                          m_providerMaintenanceTimer;
        QString                         m_socketPath;
        qint64                          m_lastFallbackLaunchMs = 0;
    };

} // namespace bb

using CAgent = bb::CAgent;
extern std::unique_ptr<CAgent> g_pAgent;
