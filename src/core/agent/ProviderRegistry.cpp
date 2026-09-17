#include "ProviderRegistry.hpp"

#include <QDateTime>
#include <QLocalSocket>
#include <QUuid>

#include <algorithm>
#include <limits>
#include <utility>

namespace bb::agent {

    namespace {

        inline constexpr qint64 PROVIDER_HEARTBEAT_TIMEOUT_MS = 15000;

    } // namespace

    ProviderRegistry::ProviderRegistry() : ProviderRegistry([] { return QDateTime::currentMSecsSinceEpoch(); }) {}

    ProviderRegistry::ProviderRegistry(NowFn nowFn) : m_nowFn(std::move(nowFn)) {}

    UIProvider ProviderRegistry::registerProvider(QLocalSocket* socket, const QJsonObject& msg, bool trusted) {
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

        if (msg.contains("priority")) {
            const int requestedPriority = msg.value("priority").toInt();
            provider.priority           = std::clamp(requestedPriority, PROVIDER_PRIORITY_MIN, PROVIDER_PRIORITY_MAX);
        } else if (provider.kind == "quickshell") {
            provider.priority = 100;
        } else if (provider.kind == "fallback") {
            provider.priority = 10;
        } else {
            provider.priority = 50;
        }

        // Trust is sticky: once attested, re-registration on the same socket cannot drop
        // it, and an unattested re-register cannot forge it.
        provider.trusted         = provider.trusted || trusted;
        provider.lastHeartbeatMs = m_nowFn();
        return provider;
    }

    bool ProviderRegistry::heartbeat(QLocalSocket* socket) {
        auto it = m_uiProviders.find(socket);
        if (it == m_uiProviders.end()) {
            return false;
        }

        it->lastHeartbeatMs = m_nowFn();
        return true;
    }

    bool ProviderRegistry::unregisterProvider(QLocalSocket* socket) {
        return m_uiProviders.remove(socket) > 0;
    }

    bool ProviderRegistry::removeSocket(QLocalSocket* socket) {
        return unregisterProvider(socket);
    }

    bool ProviderRegistry::recomputeActiveProvider() {
        const qint64  nowMs = m_nowFn();

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

            // Only daemon-launched (attested) providers are eligible to become active —
            // the active provider receives the user's secret. Untrusted registrations
            // stay in the map (so re-registration can still attest) but are never chosen.
            if (!provider.trusted) {
                ++it;
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
        return true;
    }

    bool ProviderRegistry::pruneStale() {
        return recomputeActiveProvider();
    }

    // @lat: [[provider-trust#Fail-closed authorization]]
    bool ProviderRegistry::isAuthorized(QLocalSocket* socket) const {
        // Fail closed. There is no legacy "no providers registered → anyone authorized"
        // window: the daemon launches its own trusted fallback when a session needs a
        // provider, so the authorized peer is always a daemon-launched, attested, active
        // provider — never an arbitrary same-UID socket.
        const auto it = m_uiProviders.constFind(socket);
        if (it == m_uiProviders.constEnd()) {
            return false;
        }

        return it->trusted && socket == m_activeProvider;
    }

    bool ProviderRegistry::hasActiveProvider() const {
        return m_activeProvider && m_uiProviders.contains(m_activeProvider);
    }

    QLocalSocket* ProviderRegistry::activeProvider() const {
        return m_activeProvider;
    }

    const UIProvider* ProviderRegistry::activeProviderInfo() const {
        if (!hasActiveProvider()) {
            return nullptr;
        }

        auto it = m_uiProviders.constFind(m_activeProvider);
        return (it == m_uiProviders.constEnd()) ? nullptr : &it.value();
    }

    const UIProvider* ProviderRegistry::provider(QLocalSocket* socket) const {
        auto it = m_uiProviders.constFind(socket);
        return (it == m_uiProviders.constEnd()) ? nullptr : &it.value();
    }

    bool ProviderRegistry::contains(QLocalSocket* socket) const {
        return m_uiProviders.contains(socket);
    }

    QList<QLocalSocket*> ProviderRegistry::sockets() const {
        return m_uiProviders.keys();
    }

} // namespace bb::agent
