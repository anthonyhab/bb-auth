#include "Agent.hpp"
#include "../common/Constants.hpp"
#include "RequestContext.hpp"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>
#include <QFileInfo>

#include <memory>
#include <pwd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <print>

using namespace bb;

std::unique_ptr<CAgent> g_pAgent;

namespace {

    inline constexpr int    PROVIDER_MAINTENANCE_INTERVAL_MS = 5000;
    inline constexpr qint64 FALLBACK_LAUNCH_COOLDOWN_MS      = 5000;

    QJsonObject             readBootstrapState() {
        QJsonObject   bootstrap;

        const QString stateRoot = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation);
        if (stateRoot.isEmpty()) {
            return bootstrap;
        }

        QFile stateFile(stateRoot + "/bb-auth/bootstrap-state.env");
        if (!stateFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return bootstrap;
        }

        while (!stateFile.atEnd()) {
            const QString line = QString::fromUtf8(stateFile.readLine()).trimmed();
            if (line.isEmpty() || line.startsWith('#')) {
                continue;
            }

            const qsizetype delimiter = line.indexOf('=');
            if (delimiter <= 0) {
                continue;
            }

            const QString key   = line.left(delimiter).trimmed();
            const QString value = line.mid(delimiter + 1).trimmed();
            if (key.isEmpty()) {
                continue;
            }

            if (key == "timestamp") {
                bootstrap[key] = value.toLongLong();
            } else {
                bootstrap[key] = value;
            }
        }

        const QByteArray modeOverride = qgetenv("BB_AUTH_CONFLICT_MODE");
        if (!modeOverride.isEmpty()) {
            bootstrap["mode"] = QString::fromLocal8Bit(modeOverride);
        }

        return bootstrap;
    }

} // namespace

CAgent::CAgent(QObject* parent) : QObject(parent), m_listener(new CPolkitListener(this)), m_eventRouter(m_providerRegistry, m_eventQueue) {
    m_messageRouter.registerHandler("ping", [this](QLocalSocket* socket, const QJsonObject&) {
        QJsonObject       pong{{"type", "pong"}, {"version", "2.0"}, {"capabilities", QJsonArray{"polkit", "keyring", "pinentry", "fingerprint", "fido2"}}};

        const QJsonObject bootstrap = readBootstrapState();
        if (!bootstrap.isEmpty()) {
            pong["bootstrap"] = bootstrap;
        }

        if (hasActiveProvider()) {
            if (const auto* provider = m_providerRegistry.activeProviderInfo()) {
                QJsonObject providerObj{{"id", provider->id}, {"name", provider->name}, {"kind", provider->kind}, {"priority", provider->priority}};
                pong["provider"] = providerObj;
            }
        }

        m_ipcServer.sendJson(socket, pong);
    });

    m_messageRouter.registerHandler("subscribe", [this](QLocalSocket* socket, const QJsonObject&) { handleSubscribe(socket); });
    m_messageRouter.registerHandler("next", [this](QLocalSocket* socket, const QJsonObject&) { handleNext(socket); });
    m_messageRouter.registerHandler("keyring_request", [this](QLocalSocket* socket, const QJsonObject& msg) { handleKeyringRequest(socket, msg); });
    m_messageRouter.registerHandler("pinentry_request", [this](QLocalSocket* socket, const QJsonObject& msg) { handlePinentryRequest(socket, msg); });
    m_messageRouter.registerHandler("pinentry_result", [this](QLocalSocket* socket, const QJsonObject& msg) { handlePinentryResult(socket, msg); });
    m_messageRouter.registerHandler("ui.register", [this](QLocalSocket* socket, const QJsonObject& msg) { handleUIRegister(socket, msg); });
    m_messageRouter.registerHandler("ui.heartbeat", [this](QLocalSocket* socket, const QJsonObject& msg) { handleUIHeartbeat(socket, msg); });
    m_messageRouter.registerHandler("ui.unregister", [this](QLocalSocket* socket, const QJsonObject& msg) { handleUIUnregister(socket, msg); });
    m_messageRouter.registerHandler("session.respond", [this](QLocalSocket* socket, const QJsonObject& msg) { handleRespond(socket, msg); });
    m_messageRouter.registerHandler("session.cancel", [this](QLocalSocket* socket, const QJsonObject& msg) { handleCancel(socket, msg); });
}

CAgent::~CAgent() {}

#include <PolkitQt1/Subject>

bool CAgent::start(QCoreApplication& app, const QString& socketPath) {
    m_socketPath = socketPath;

    PolkitQt1::UnixSessionSubject subject(getpid());
    if (!m_listener->registerListener(subject, "/org/kde/PolicyKit1/AuthenticationAgent")) {
        std::print(stderr, "Failed to register as Polkit agent listener\n");
        return false;
    }

    std::print("Polkit listener registered successfully\n");

    // Connect Polkit signals
    connect(m_listener.data(), &CPolkitListener::completed, this, &CAgent::onPolkitCompleted);

    // Setup IPC server
    m_ipcServer.setMessageHandler([this](QLocalSocket* socket, const QString& type, const QJsonObject& msg) { handleMessage(socket, type, msg); });

    QObject::connect(&m_ipcServer, &bb::IpcServer::clientDisconnected, [this](QLocalSocket* socket) { onClientDisconnected(socket); });

    m_providerMaintenanceTimer.setInterval(PROVIDER_MAINTENANCE_INTERVAL_MS);
    m_providerMaintenanceTimer.setSingleShot(false);
    QObject::connect(&m_providerMaintenanceTimer, &QTimer::timeout, [this]() { pruneStaleProviders(); });
    m_providerMaintenanceTimer.start();

    if (!m_ipcServer.start(socketPath)) {
        std::print(stderr, "Failed to start IPC server on {}\n", socketPath.toStdString());
        return false;
    }

    std::print("Agent started on {}\n", socketPath.toStdString());
    return app.exec() == 0;
}

void CAgent::onClientDisconnected(QLocalSocket* socket) {
    if (m_subscribers.removeOne(socket)) {
        qDebug() << "Subscriber removed, remaining:" << m_subscribers.size();
    }

    if (m_providerRegistry.removeSocket(socket)) {
        qDebug() << "UI provider disconnected:" << socket;
        if (m_providerRegistry.recomputeActiveProvider()) {
            emitProviderStatus();
        }
    }

    m_eventQueue.removeWaiter(socket);
    m_keyringManager.cleanupForSocket(socket);
    m_pinentryManager.cleanupForSocket(socket);

    if (!hasActiveProvider() && !m_sessionStore.empty()) {
        ensureFallbackUiRunning("provider-disconnected");
    }
}

void CAgent::handleMessage(QLocalSocket* socket, const QString& type, const QJsonObject& msg) {
    if (!m_messageRouter.dispatch(socket, type, msg)) {
        m_ipcServer.sendJson(socket, QJsonObject{{"type", "error"}, {"message", "Unknown type"}});
    }
}

void CAgent::handleNext(QLocalSocket* socket) {
    if (m_eventQueue.isEmpty()) {
        m_eventQueue.subscribeNext(socket);
        return;
    }

    m_ipcServer.sendJson(socket, m_eventQueue.takeNext());
}

void CAgent::handleSubscribe(QLocalSocket* socket) {
    if (!m_subscribers.contains(socket)) {
        m_subscribers.append(socket);
        qDebug() << "Subscriber added, total:" << m_subscribers.size();
    }

    const bool isRegisteredProvider        = m_providerRegistry.contains(socket);
    const bool isActiveProvider            = isRegisteredProvider && (socket == m_providerRegistry.activeProvider());
    const bool canReceiveInteractiveEvents = !isRegisteredProvider || isActiveProvider;

    if (canReceiveInteractiveEvents) {
        for (const auto& [cookie, session] : m_sessionStore.sessions()) {
            m_ipcServer.sendJson(socket, session->toCreatedEvent());
            m_ipcServer.sendJson(socket, session->toUpdatedEvent());
        }
    }

    QJsonObject subscribedMsg{{"type", "subscribed"}, {"sessionCount", canReceiveInteractiveEvents ? static_cast<int>(m_sessionStore.size()) : 0}};

    if (isRegisteredProvider) {
        subscribedMsg["active"] = isActiveProvider;
    }

    m_ipcServer.sendJson(socket, subscribedMsg);
}

void CAgent::handleKeyringRequest(QLocalSocket* socket, const QJsonObject& msg) {
    pid_t peerPid = bb::IpcServer::getPeerPid(socket);
    m_keyringManager.handleRequest(msg, socket, peerPid);
}

void CAgent::handlePinentryRequest(QLocalSocket* socket, const QJsonObject& msg) {
    pid_t peerPid = bb::IpcServer::getPeerPid(socket);
    m_pinentryManager.handleRequest(msg, socket, peerPid);
}

void CAgent::handlePinentryResult(QLocalSocket* socket, const QJsonObject& msg) {
    pid_t       peerPid = bb::IpcServer::getPeerPid(socket);
    QJsonObject result  = m_pinentryManager.handleResult(msg, peerPid);
    m_ipcServer.sendJson(socket, result);
}

void CAgent::handleUIRegister(QLocalSocket* socket, const QJsonObject& msg) {
    const auto provider              = m_providerRegistry.registerProvider(socket, msg);
    const bool activeProviderChanged = m_providerRegistry.recomputeActiveProvider();
    const bool nowActive             = socket == m_providerRegistry.activeProvider();

    m_ipcServer.sendJson(socket, QJsonObject{{"type", "ui.registered"}, {"id", provider.id}, {"active", nowActive}, {"priority", provider.priority}});

    if (activeProviderChanged || nowActive) {
        emitProviderStatus();
    }
}
void CAgent::handleUIHeartbeat(QLocalSocket* socket, const QJsonObject& msg) {
    Q_UNUSED(msg)

    if (!m_providerRegistry.heartbeat(socket)) {
        m_ipcServer.sendJson(socket, QJsonObject{{"type", "error"}, {"message", "Provider not registered"}});
        return;
    }

    if (m_providerRegistry.recomputeActiveProvider()) {
        emitProviderStatus();
    }

    m_ipcServer.sendJson(socket, QJsonObject{{"type", "ok"}, {"active", socket == m_providerRegistry.activeProvider()}});
}
void CAgent::handleUIUnregister(QLocalSocket* socket, const QJsonObject& msg) {
    Q_UNUSED(msg)

    if (!m_providerRegistry.unregisterProvider(socket)) {
        m_ipcServer.sendJson(socket, QJsonObject{{"type", "error"}, {"message", "Provider not registered"}});
        return;
    }

    if (m_providerRegistry.recomputeActiveProvider()) {
        emitProviderStatus();
    }
    m_ipcServer.sendJson(socket, QJsonObject{{"type", "ok"}});

    if (!hasActiveProvider() && !m_sessionStore.empty()) {
        ensureFallbackUiRunning("provider-unregistered");
    }
}

void CAgent::handleRespond(QLocalSocket* socket, const QJsonObject& msg) {
    const QString cookie   = msg.value("id").toString();
    const QString response = msg.value("response").toString();

    if (!isAuthorizedProviderSocket(socket)) {
        m_ipcServer.sendJson(socket, QJsonObject{{"type", "error"}, {"message", "Not active UI provider"}});
        return;
    }

    if (m_keyringManager.hasPendingRequest(cookie)) {
        QLocalSocket* origSocket = m_keyringManager.getSocketForRequest(cookie);
        QJsonObject   reply      = m_keyringManager.handleResponse(cookie, response);
        if (origSocket)
            m_ipcServer.sendJson(origSocket, reply, true);
        m_ipcServer.sendJson(socket, QJsonObject{{"type", "ok"}});
        return;
    }

    if (m_pinentryManager.hasPendingInput(cookie)) {
        QLocalSocket* origSocket = m_pinentryManager.getSocketForPendingInput(cookie);
        auto          result     = m_pinentryManager.handleResponse(cookie, response);
        if (!origSocket || result.socketResponse.value("type").toString() == "error") {
            const QString message = result.socketResponse.value("message").toString();
            m_ipcServer.sendJson(socket, QJsonObject{{"type", "error"}, {"message", message.isEmpty() ? "Invalid pinentry session state" : message}});
            return;
        }

        m_ipcServer.sendJson(origSocket, result.socketResponse, true);
        m_ipcServer.sendJson(socket, QJsonObject{{"type", "ok"}});
        return;
    }

    if (m_pinentryManager.hasRequest(cookie)) {
        m_ipcServer.sendJson(socket, QJsonObject{{"type", "error"}, {"message", "Session is not accepting input"}});
        return;
    }

    Session* session = getSession(cookie);
    if (!session) {
        m_ipcServer.sendJson(socket, QJsonObject{{"type", "error"}, {"message", "Unknown session"}});
        return;
    }

    if (session->source() != Session::Source::Polkit) {
        m_ipcServer.sendJson(socket, QJsonObject{{"type", "error"}, {"message", "Session is not awaiting direct response"}});
        return;
    }

    m_listener->submitPassword(cookie, response);
    m_ipcServer.sendJson(socket, QJsonObject{{"type", "ok"}});
}

void CAgent::handleCancel(QLocalSocket* socket, const QJsonObject& msg) {
    const QString cookie = msg.value("id").toString();

    if (!isAuthorizedProviderSocket(socket)) {
        m_ipcServer.sendJson(socket, QJsonObject{{"type", "error"}, {"message", "Not active UI provider"}});
        return;
    }

    if (m_keyringManager.hasPendingRequest(cookie)) {
        QLocalSocket* origSocket = m_keyringManager.getSocketForRequest(cookie);
        QJsonObject   reply      = m_keyringManager.handleCancel(cookie);
        if (origSocket)
            m_ipcServer.sendJson(origSocket, reply);
        m_ipcServer.sendJson(socket, QJsonObject{{"type", "ok"}});
        return;
    }

    if (m_pinentryManager.hasRequest(cookie)) {
        QLocalSocket* origSocket = m_pinentryManager.getSocketForPendingInput(cookie);
        QJsonObject   reply      = m_pinentryManager.handleCancel(cookie);
        if (reply.value("type").toString() == "error") {
            m_ipcServer.sendJson(socket, reply);
            return;
        }

        if (origSocket) {
            m_ipcServer.sendJson(origSocket, reply);
        }
        m_ipcServer.sendJson(socket, QJsonObject{{"type", "ok"}});
        return;
    }

    Session* session = getSession(cookie);
    if (!session) {
        m_ipcServer.sendJson(socket, QJsonObject{{"type", "error"}, {"message", "Unknown session"}});
        return;
    }

    if (session->source() != Session::Source::Polkit) {
        m_ipcServer.sendJson(socket, QJsonObject{{"type", "error"}, {"message", "Session is not cancellable from this path"}});
        return;
    }

    m_listener->cancelPending(cookie);
    m_ipcServer.sendJson(socket, QJsonObject{{"type", "ok"}});
}

void CAgent::emitSessionEvent(const QJsonObject& event) {
    m_eventRouter.route(event, m_subscribers, [this](QLocalSocket* socket, const QJsonObject& routedEvent) { m_ipcServer.sendJson(socket, routedEvent); });
}

void CAgent::onPolkitRequest(const QString& cookie, const QString& message, [[maybe_unused]] const QString& iconName, const QString& actionId, const QString& user,
                             const PolkitQt1::Details& details) {
    qDebug() << "POLKIT REQUEST" << cookie;

    bb::Session::Context ctx;
    ctx.message  = message;
    ctx.actionId = actionId;
    ctx.user     = user;

    auto pid = RequestContextHelper::extractSubjectPid(details);
    if (pid) {
        auto proc = RequestContextHelper::readProc(*pid);
        if (proc) {
            auto actor                   = RequestContextHelper::resolveRequestorFromSubject(*proc, getuid());
            ctx.requestor.name           = actor.displayName;
            ctx.requestor.icon           = actor.iconName;
            ctx.requestor.fallbackLetter = actor.fallbackLetter;
            ctx.requestor.fallbackKey    = actor.fallbackKey;
            ctx.requestor.pid            = *pid;
        }
    }

    if (ctx.requestor.name.isEmpty()) {
        ctx.requestor.name           = "Unknown";
        ctx.requestor.fallbackLetter = "?";
        ctx.requestor.fallbackKey    = "unknown";
    }

    createSession(cookie, bb::Session::Source::Polkit, ctx);
}

void CAgent::onSessionRequest(const QString& cookie, const QString& prompt, bool echo) {
    const auto updated = m_sessionStore.updatePrompt(cookie, prompt, echo, true);
    if (!updated) {
        qWarning() << "Session not found:" << cookie;
        return;
    }

    emitSessionEvent(*updated);
}
void CAgent::onSessionComplete(const QString& cookie, bool success) {
    const auto closed = m_sessionStore.closeSession(cookie, success ? bb::Session::Result::Success : bb::Session::Result::Cancelled);
    if (!closed) {
        qWarning() << "Session not found:" << cookie;
        return;
    }

    emitSessionEvent(*closed);

    if (m_sessionStore.empty()) {
        if (m_providerRegistry.recomputeActiveProvider()) {
            emitProviderStatus();
        }
    }
}
void CAgent::onSessionRetry(const QString& cookie, const QString& error) {
    const auto updated = m_sessionStore.updateError(cookie, error);
    if (!updated) {
        return;
    }

    emitSessionEvent(*updated);
}
void CAgent::onSessionInfo(const QString& cookie, const QString& info) {
    const auto updated = m_sessionStore.updateInfo(cookie, info);
    if (!updated) {
        return;
    }

    emitSessionEvent(*updated);
}

void CAgent::onPolkitCompleted([[maybe_unused]] bool gainedAuthorization) {}
// Centralized session management
void CAgent::createSession(const QString& id, Session::Source source, Session::Context ctx) {
    const QJsonObject createdEvent = m_sessionStore.createSession(id, source, ctx);
    emitSessionEvent(createdEvent);
    if (!hasActiveProvider()) {
        ensureFallbackUiRunning("session-created");
    }
}
void CAgent::updateSessionPrompt(const QString& id, const QString& prompt, bool echo, bool clearError) {
    const auto updated = m_sessionStore.updatePrompt(id, prompt, echo, clearError);
    if (!updated) {
        qWarning() << "updateSessionPrompt: Session not found:" << id;
        return;
    }

    emitSessionEvent(*updated);
}
void CAgent::updateSessionError(const QString& id, const QString& error) {
    const auto updated = m_sessionStore.updateError(id, error);
    if (!updated) {
        qWarning() << "updateSessionError: Session not found:" << id;
        return;
    }

    emitSessionEvent(*updated);
}
void CAgent::updateSessionPinentryRetry(const QString& id, int curRetry, int maxRetries) {
    if (!m_sessionStore.updatePinentryRetry(id, curRetry, maxRetries)) {
        Session* session = m_sessionStore.getSession(id);
        if (!session) {
            qWarning() << "updateSessionPinentryRetry: Session not found:" << id;
        } else {
            qWarning() << "updateSessionPinentryRetry: Not a pinentry session:" << id;
        }
    }
}
QJsonObject CAgent::closeSession(const QString& id, Session::Result result, bool deferred) {
    const auto closed = m_sessionStore.closeSession(id, result);
    if (!closed) {
        qWarning() << "closeSession: Session not found:" << id;
        return QJsonObject{};
    }

    if (m_sessionStore.empty()) {
        if (m_providerRegistry.recomputeActiveProvider()) {
            emitProviderStatus();
        }
    }
    if (!deferred) {
        emitSessionEvent(*closed);
        return QJsonObject{};
    }
    return *closed;
}
Session* CAgent::getSession(const QString& id) {
    return m_sessionStore.getSession(id);
}

bool CAgent::isAuthorizedProviderSocket(QLocalSocket* socket) const {
    return m_providerRegistry.isAuthorized(socket);
}
bool CAgent::hasActiveProvider() const {
    return m_providerRegistry.hasActiveProvider();
}
void CAgent::pruneStaleProviders() {
    if (m_providerRegistry.pruneStale()) {
        emitProviderStatus();
    }
    if (!hasActiveProvider() && !m_sessionStore.empty()) {
        ensureFallbackUiRunning("provider-prune");
    }
}
void CAgent::emitProviderStatus() {
    QJsonObject status{{"type", "ui.active"}, {"active", hasActiveProvider()}};
    if (const auto* provider = m_providerRegistry.activeProviderInfo()) {
        status["id"]       = provider->id;
        status["name"]     = provider->name;
        status["kind"]     = provider->kind;
        status["priority"] = provider->priority;
    }

    QSet<QLocalSocket*> sent;

    for (QLocalSocket* socket : m_providerRegistry.sockets()) {
        if (socket && socket->isValid()) {
            m_ipcServer.sendJson(socket, status);
            sent.insert(socket);
        }
    }
    for (auto* subscriber : m_subscribers) {
        if (subscriber && subscriber->isValid() && !sent.contains(subscriber)) {
            m_ipcServer.sendJson(subscriber, status);
        }
    }
}

void CAgent::ensureFallbackUiRunning(const QString& reason) {
    if (hasActiveProvider()) {
        return;
    }

    {
        QProcess probe;
        probe.start("pgrep", QStringList{"-u", QString::number(getuid()), "-f", "bb-auth-fallback"});
        if (probe.waitForFinished(500) && probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0) {
            return;
        }
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if ((nowMs - m_lastFallbackLaunchMs) < FALLBACK_LAUNCH_COOLDOWN_MS) {
        return;
    }

    QString fallbackPath = QString::fromLocal8Bit(qgetenv("BB_AUTH_FALLBACK_PATH"));
    if (fallbackPath.isEmpty()) {
        fallbackPath = QCoreApplication::applicationDirPath() + "/bb-auth-fallback";
    }

    const QFileInfo info(fallbackPath);
    if (!info.exists() || !info.isExecutable()) {
        qWarning() << "Fallback UI binary missing or not executable:" << fallbackPath;
        return;
    }

    QStringList args;
    if (!m_socketPath.isEmpty()) {
        args << "--socket" << m_socketPath;
    }

    const bool launched = QProcess::startDetached(fallbackPath, args);
    if (launched) {
        m_lastFallbackLaunchMs = nowMs;
        qInfo() << "Launched fallback UI due to" << reason;
    } else {
        qWarning() << "Failed to launch fallback UI:" << fallbackPath;
    }
}
