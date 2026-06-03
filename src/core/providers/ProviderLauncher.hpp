#pragma once

#include "ProviderManifest.hpp"

#include <QHash>
#include <QProcessEnvironment>

#include <functional>

namespace bb::providers {

    struct LaunchAttemptResult {
        bool    attempted   = false;
        bool    launched    = false;
        qint64  launchedPid = 0; // pid of the launched process (0 if not launched)
        QString providerId;
        QString executable;
        QString detail;
    };

    class ProviderLauncher {
      public:
        using NowFn          = std::function<qint64()>;
        // Returns the launched process pid, or 0 on failure. The pid anchors provider
        // trust (see ProviderTrustStore): the daemon records it and only authorizes a
        // registration whose SO_PEERCRED peer matches a launched pid.
        using StartProcessFn = std::function<qint64(const QString&, const QStringList&, const QProcessEnvironment&)>;

        explicit ProviderLauncher(NowFn nowFn = {}, StartProcessFn startProcessFn = {});

        // eager: bring a resident provider up before any prompt arrives — ignores the
        // pending-session gate and only ever selects a real autostart provider (never the
        // legacy/built-in fallback, which stays an on-demand safety net).
        LaunchAttemptResult tryLaunch(const QList<ProviderManifest>& manifests, const QString& socketPath, const QString& reason, bool hasActiveProvider, bool hasPendingSessions,
                                      const QString& legacyFallbackPath, const QString& defaultFallbackPath, bool eager = false);

      private:
        struct RetryState {
            int    failures       = 0;
            qint64 nextEligibleMs = 0;
        };

        struct SelectedCandidate {
            QString             id;
            QString             displayName;
            QString             exec;
            QStringList         args;
            QProcessEnvironment env;
        };

        static qint64              defaultNowMs();
        static qint64              defaultStartProcess(const QString& program, const QStringList& args, const QProcessEnvironment& env);
        static QString             resolveExecutable(const QString& exec);
        static bool                isExecutableFile(const QString& path);
        static QProcessEnvironment mergeEnvironment(const QJsonObject& envObject);

        SelectedCandidate          selectCandidate(const QList<ProviderManifest>& manifests, bool eager, const QString& legacyFallbackPath, const QString& defaultFallbackPath,
                                                   const QString& socketPath, QString& selectionError) const;

        bool                       canAttempt(const QString& id, qint64 nowMs, QString& detail) const;
        void                       markSuccess(const QString& id);
        void                       markFailure(const QString& id, qint64 nowMs);

        NowFn                      m_nowFn;
        StartProcessFn             m_startProcessFn;
        QHash<QString, RetryState> m_retryByProvider;
    };

} // namespace bb::providers
