#pragma once

#include "ProviderManifest.hpp"

#include <QHash>
#include <QProcessEnvironment>

#include <functional>

namespace bb::providers {

    struct LaunchAttemptResult {
        bool    attempted = false;
        bool    launched  = false;
        QString providerId;
        QString executable;
        QString detail;
    };

    class ProviderLauncher {
      public:
        using NowFn          = std::function<qint64()>;
        using StartProcessFn = std::function<bool(const QString&, const QStringList&, const QProcessEnvironment&)>;

        explicit ProviderLauncher(NowFn nowFn = {}, StartProcessFn startProcessFn = {});

        LaunchAttemptResult tryLaunch(const QList<ProviderManifest>& manifests, const QString& socketPath, const QString& reason, bool hasActiveProvider, bool hasPendingSessions,
                                      const QString& legacyFallbackPath, const QString& defaultFallbackPath);

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
        static bool                defaultStartProcess(const QString& program, const QStringList& args, const QProcessEnvironment& env);
        static QString             resolveExecutable(const QString& exec);
        static bool                isExecutableFile(const QString& path);
        static QProcessEnvironment mergeEnvironment(const QJsonObject& envObject);

        SelectedCandidate          selectCandidate(const QList<ProviderManifest>& manifests, const QString& legacyFallbackPath, const QString& defaultFallbackPath,
                                                   const QString& socketPath, QString& selectionError) const;

        bool                       canAttempt(const QString& id, qint64 nowMs, QString& detail) const;
        void                       markSuccess(const QString& id);
        void                       markFailure(const QString& id, qint64 nowMs);

        NowFn                      m_nowFn;
        StartProcessFn             m_startProcessFn;
        QHash<QString, RetryState> m_retryByProvider;
    };

} // namespace bb::providers
