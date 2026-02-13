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
#include <limits>
#include <pwd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <print>

using namespace bb;

std::unique_ptr<CAgent> g_pAgent;

namespace {

    inline constexpr qint64 PROVIDER_HEARTBEAT_TIMEOUT_MS    = 15000;
    inline constexpr int    PROVIDER_MAINTENANCE_INTERVAL_MS = 5000;
    inline constexpr qint64 FALLBACK_LAUNCH_COOLDOWN_MS      = 5000;
    inline constexpr int    EVENT_QUEUE_MAX_SIZE             = 256;

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

CAgent::CAgent(QObject* parent) : QObject(parent), m_listener(new CPolkitListener(this)) {}

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

    if (m_uiProviders.remove(socket) > 0) {
        qDebug() << "UI provider disconnected:" << socket;
        recomputeActiveProvider();
    }

    nextWaiters.removeAll(socket);
    m_keyringManager.cleanupForSocket(socket);
    m_pinentryManager.cleanupForSocket(socket);

    if (!hasActiveProvider() && !m_sessions.empty()) {
        ensureFallbackUiRunning("provider-disconnected");
    }
}

void CAgent::handleMessage(QLocalSocket* socket, const QString& type, const QJsonObject& msg) {
    if (type == "ping") {
        QJsonObject       pong{{"type", "pong"}, {"version", "2.0"}, {"capabilities", QJsonArray{"polkit", "keyring", "pinentry", "fingerprint", "fido2"}}};

        const QJsonObject bootstrap = readBootstrapState();
        if (!bootstrap.isEmpty()) {
            pong["bootstrap"] = bootstrap;
        }

        if (hasActiveProvider()) {
            const auto& provider = m_uiProviders[m_activeProvider];
            QJsonObject providerObj{{"id", provider.id}, {"name", provider.name}, {"kind", provider.kind}, {"priority", provider.priority}};
            pong["provider"] = providerObj;
        }

        m_ipcServer.sendJson(socket, pong);
    } else if (type == "subscribe") {
        handleSubscribe(socket);
    } else if (type == "next") {
        handleNext(socket);
    } else if (type == "keyring_request") {
        handleKeyringRequest(socket, msg);
    } else if (type == "pinentry_request") {
        handlePinentryRequest(socket, msg);
    } else if (type == "pinentry_result") {
        handlePinentryResult(socket, msg);
    } else if (type == "ui.register") {
        handleUIRegister(socket, msg);
    } else if (type == "ui.heartbeat") {
        handleUIHeartbeat(socket, msg);
    } else if (type == "ui.unregister") {
        handleUIUnregister(socket, msg);
    } else if (type == "session.respond") {
        handleRespond(socket, msg);
    } else if (type == "session.cancel") {
        handleCancel(socket, msg);
    } else {
        m_ipcServer.sendJson(socket, QJsonObject{{"type", "error"}, {"message", "Unknown type"}});
    }
}

void CAgent::handleNext(QLocalSocket* socket) {
    if (eventQueue.isEmpty()) {
        nextWaiters.append(socket);
        return;
    }

    QJsonObject nextEvent = eventQueue.takeFirst();
    m_ipcServer.sendJson(socket, nextEvent);
}

void CAgent::handleSubscribe(QLocalSocket* socket) {
    if (!m_subscribers.contains(socket)) {
        m_subscribers.append(socket);
        qDebug() << "Subscriber added, total:" << m_subscribers.size();
    }

    const bool isRegisteredProvider        = m_uiProviders.contains(socket);
    const bool isActiveProvider            = isRegisteredProvider && (socket == m_activeProvider);
    const bool canReceiveInteractiveEvents = !isRegisteredProvider || isActiveProvider;

    if (canReceiveInteractiveEvents) {
        for (const auto& [cookie, session] : m_sessions) {
            m_ipcServer.sendJson(socket, session->toCreatedEvent());
            m_ipcServer.sendJson(socket, session->toUpdatedEvent());
        }
    }

    QJsonObject subscribedMsg{{"type", "subscribed"}, {"sessionCount", canReceiveInteractiveEvents ? static_cast<int>(m_sessions.size()) : 0}};

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
    auto& provider = m_uiProviders[socket];

    if (provider.id.isEmpty()) {
        provider.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    provider.name = msg.value("name").toString();
    if (provider.name.isEmpty()) {
        provider.name = "unknown";
    }

    provider.kind = msg.value("kind").toString();
    if (provider.kind.isEmpty()) {
        provider.kind = provider.name;
    }

    const int requestedPriority = msg.value("priority").toInt();
    if (msg.contains("priority")) {
        provider.priority = requestedPriority;
    } else if (provider.kind == "quickshell") {
        provider.priority = 100;
    } else if (provider.kind == "fallback") {
        provider.priority = 10;
    } else {
        provider.priority = 50;
    }

    provider.lastHeartbeatMs         = QDateTime::currentMSecsSinceEpoch();
    const bool activeProviderChanged = recomputeActiveProvider(false);

    m_ipcServer.sendJson(socket, QJsonObject{{"type", "ui.registered"}, {"id", provider.id}, {"active", socket == m_activeProvider}, {"priority", provider.priority}});

    if (activeProviderChanged || socket == m_activeProvider) {
        emitProviderStatus();
    }
}

void CAgent::handleUIHeartbeat(QLocalSocket* socket, const QJsonObject& msg) {
    Q_UNUSED(msg)

    auto it = m_uiProviders.find(socket);
    if (it == m_uiProviders.end()) {
        m_ipcServer.sendJson(socket, QJsonObject{{"type", "error"}, {"message", "Provider not registered"}});
        return;
    }

    it->lastHeartbeatMs = QDateTime::currentMSecsSinceEpoch();
    recomputeActiveProvider();

    m_ipcServer.sendJson(socket, QJsonObject{{"type", "ok"}, {"active", socket == m_activeProvider}});
}

void CAgent::handleUIUnregister(QLocalSocket* socket, const QJsonObject& msg) {
    Q_UNUSED(msg)

    if (m_uiProviders.remove(socket) == 0) {
        m_ipcServer.sendJson(socket, QJsonObject{{"type", "error"}, {"message", "Provider not registered"}});
        return;
    }

    recomputeActiveProvider();
    m_ipcServer.sendJson(socket, QJsonObject{{"type", "ok"}});

    if (!hasActiveProvider() && !m_sessions.empty()) {
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
    const QString eventType = event.value("type").toString();

    if (isSessionEventForProviderRouting(event) && hasActiveProvider()) {
        if (m_activeProvider && m_activeProvider->isValid()) {
            qDebug() << "Routing event" << eventType << "to active provider";
            m_ipcServer.sendJson(m_activeProvider, event);
        }
    } else {
        qDebug() << "Broadcasting event:" << eventType << "to" << m_subscribers.size() << "subscribers";

        for (QLocalSocket* subscriber : m_subscribers) {
            if (subscriber && subscriber->isValid()) {
                m_ipcServer.sendJson(subscriber, event);
            }
        }
    }

    // Cap queue size to prevent unbounded growth during high event churn
    if (eventQueue.size() >= EVENT_QUEUE_MAX_SIZE) {
        eventQueue.dequeue(); // Drop oldest event
    }
    eventQueue.enqueue(event);
    processNextWaiter();
}

void CAgent::processNextWaiter() {
    while (!nextWaiters.isEmpty() && !eventQueue.isEmpty()) {
        QLocalSocket* socket = nextWaiters.takeFirst();
        handleNext(socket);
    }
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
    auto it = m_sessions.find(cookie);
    if (it == m_sessions.end()) {
        qWarning() << "Session not found:" << cookie;
        return;
    }

    it->second->setPrompt(prompt, echo);
    emitSessionEvent(it->second->toUpdatedEvent());
}

void CAgent::onSessionComplete(const QString& cookie, bool success) {
    auto it = m_sessions.find(cookie);
    if (it == m_sessions.end()) {
        qWarning() << "Session not found:" << cookie;
        return;
    }

    it->second->close(success ? bb::Session::Result::Success : bb::Session::Result::Cancelled);
    emitSessionEvent(it->second->toClosedEvent());
    m_sessions.erase(it);

    if (m_sessions.empty()) {
        recomputeActiveProvider();
    }
}

void CAgent::onSessionRetry(const QString& cookie, const QString& error) {
    auto it = m_sessions.find(cookie);
    if (it == m_sessions.end())
        return;

    it->second->setError(error);
    emitSessionEvent(it->second->toUpdatedEvent());
}

void CAgent::onSessionInfo(const QString& cookie, const QString& info) {
    auto it = m_sessions.find(cookie);
    if (it == m_sessions.end())
        return;

    it->second->setInfo(info);
    emitSessionEvent(it->second->toUpdatedEvent());
}

void CAgent::onPolkitCompleted([[maybe_unused]] bool gainedAuthorization) {}

// Centralized session management
void CAgent::createSession(const QString& id, Session::Source source, Session::Context ctx) {
    auto              session      = std::make_unique<bb::Session>(id, source, ctx);
    const QJsonObject createdEvent = session->toCreatedEvent();
    m_sessions[id]                 = std::move(session);
    emitSessionEvent(createdEvent);

    if (!hasActiveProvider()) {
        ensureFallbackUiRunning("session-created");
    }
}

void CAgent::updateSessionPrompt(const QString& id, const QString& prompt, bool echo, bool clearError) {
    auto it = m_sessions.find(id);
    if (it == m_sessions.end()) {
        qWarning() << "updateSessionPrompt: Session not found:" << id;
        return;
    }
    it->second->setPrompt(prompt, echo, clearError);
    emitSessionEvent(it->second->toUpdatedEvent());
}

void CAgent::updateSessionError(const QString& id, const QString& error) {
    auto it = m_sessions.find(id);
    if (it == m_sessions.end()) {
        qWarning() << "updateSessionError: Session not found:" << id;
        return;
    }
    it->second->setError(error);
    emitSessionEvent(it->second->toUpdatedEvent());
}

void CAgent::updateSessionPinentryRetry(const QString& id, int curRetry, int maxRetries) {
    auto it = m_sessions.find(id);
    if (it == m_sessions.end()) {
        qWarning() << "updateSessionPinentryRetry: Session not found:" << id;
        return;
    }

    if (it->second->source() != Session::Source::Pinentry) {
        qWarning() << "updateSessionPinentryRetry: Not a pinentry session:" << id;
        return;
    }

    it->second->setPinentryRetry(curRetry, maxRetries);
}

QJsonObject CAgent::closeSession(const QString& id, Session::Result result, bool deferred) {
    auto it = m_sessions.find(id);
    if (it == m_sessions.end()) {
        qWarning() << "closeSession: Session not found:" << id;
        return QJsonObject{};
    }

    it->second->close(result);
    QJsonObject event = it->second->toClosedEvent();
    m_sessions.erase(it);

    if (m_sessions.empty()) {
        recomputeActiveProvider();
    }

    if (!deferred) {
        emitSessionEvent(event);
        return QJsonObject{};
    }
    return event;
}

Session* CAgent::getSession(const QString& id) {
    auto it = m_sessions.find(id);
    return (it != m_sessions.end()) ? it->second.get() : nullptr;
}

bool CAgent::isSessionEventForProviderRouting(const QJsonObject& event) const {
    const QString type = event.value("type").toString();
    return type.startsWith("session.");
}

bool CAgent::isAuthorizedProviderSocket(QLocalSocket* socket) const {
    if (m_uiProviders.isEmpty()) {
        return true;
    }

    if (!m_uiProviders.contains(socket)) {
        return false;
    }

    return socket == m_activeProvider;
}

bool CAgent::hasActiveProvider() const {
    return m_activeProvider && m_uiProviders.contains(m_activeProvider);
}

bool CAgent::recomputeActiveProvider(bool emitStatusChange) {
    const qint64  nowMs = QDateTime::currentMSecsSinceEpoch();

    QLocalSocket* bestSocket    = nullptr;
    int           bestPriority  = std::numeric_limits<int>::min();
    qint64        bestHeartbeat = 0;

    for (auto it = m_uiProviders.begin(); it != m_uiProviders.end();) {
        QLocalSocket* socket   = it.key();
        const auto&   provider = it.value();

        const bool    socketInvalid = (!socket || socket->state() != QLocalSocket::ConnectedState);
        const bool    stale         = (nowMs - provider.lastHeartbeatMs) > PROVIDER_HEARTBEAT_TIMEOUT_MS;
        if (socketInvalid || stale) {
            it = m_uiProviders.erase(it);
            continue;
        }

        if (!bestSocket || provider.priority > bestPriority || (provider.priority == bestPriority && provider.lastHeartbeatMs > bestHeartbeat)) {
            bestSocket    = socket;
            bestPriority  = provider.priority;
            bestHeartbeat = provider.lastHeartbeatMs;
        }

        ++it;
    }

    if (m_activeProvider == bestSocket) {
        return false;
    }

    m_activeProvider = bestSocket;
    if (emitStatusChange) {
        emitProviderStatus();
    }

    return true;
}

void CAgent::pruneStaleProviders() {
    recomputeActiveProvider();

    if (!hasActiveProvider() && !m_sessions.empty()) {
        ensureFallbackUiRunning("provider-prune");
    }
}

void CAgent::emitProviderStatus() {
    QJsonObject status{{"type", "ui.active"}, {"active", hasActiveProvider()}};

    if (hasActiveProvider()) {
        const auto& provider = m_uiProviders[m_activeProvider];
        status["id"]         = provider.id;
        status["name"]       = provider.name;
        status["kind"]       = provider.kind;
        status["priority"]   = provider.priority;
    }

    QSet<QLocalSocket*> sent;

    for (auto it = m_uiProviders.begin(); it != m_uiProviders.end(); ++it) {
        QLocalSocket* socket = it.key();
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
