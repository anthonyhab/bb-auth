#pragma once

#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <functional>

class QLocalSocket;

namespace bb::agent {

    struct UIProvider {
        QString id;
        QString name;
        QString kind;
        int     priority        = 0;
        qint64  lastHeartbeatMs = 0;
        // True only when the registering peer was proven to be a daemon-launched process
        // (see ProviderTrustStore). Untrusted providers may register but can never become
        // active or be authorized to receive secrets.
        bool    trusted = false;
    };

    // Lowest/highest priority a provider may request. Priority drives active-provider
    // selection, so an unbounded value would let any registration force itself active;
    // requests are clamped into this range. Matches PROVIDER_CONTRACT.md.
    inline constexpr int PROVIDER_PRIORITY_MIN = -1000;
    inline constexpr int PROVIDER_PRIORITY_MAX = 1000;

    class ProviderRegistry {
      public:
        using NowFn = std::function<qint64()>;

        ProviderRegistry();
        explicit ProviderRegistry(NowFn nowFn);

        // trusted: whether the registering peer was attested as daemon-launched. Trust is
        // sticky across re-registration on the same socket (a provider cannot lose trust
        // by re-sending ui.register, nor gain it without a fresh attestation).
        UIProvider           registerProvider(QLocalSocket* socket, const QJsonObject& msg, bool trusted);
        bool                 heartbeat(QLocalSocket* socket);
        bool                 unregisterProvider(QLocalSocket* socket);
        bool                 removeSocket(QLocalSocket* socket);
        bool                 recomputeActiveProvider();
        bool                 pruneStale();

        bool                 isAuthorized(QLocalSocket* socket) const;
        bool                 hasActiveProvider() const;

        QLocalSocket*        activeProvider() const;
        const UIProvider*    activeProviderInfo() const;
        const UIProvider*    provider(QLocalSocket* socket) const;
        bool                 contains(QLocalSocket* socket) const;
        QList<QLocalSocket*> sockets() const;

      private:
        NowFn                            m_nowFn;

        QHash<QLocalSocket*, UIProvider> m_uiProviders;
        QPointer<QLocalSocket>           m_activeProvider;
    };

} // namespace bb::agent
