#include "ProviderLauncher.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>

#include <algorithm>

namespace bb::providers {

    namespace {

        inline constexpr qint64 BASE_BACKOFF_MS = 250;
        inline constexpr qint64 MAX_BACKOFF_MS  = 5000;

        inline constexpr auto   LEGACY_ENV_ID     = "__legacy_env__";
        inline constexpr auto   LEGACY_DEFAULT_ID = "__legacy_default__";

        qint64                  computeBackoffMs(int failures) {
            const qint64 exp    = BASE_BACKOFF_MS << std::min(failures, 8);
            const qint64 cap    = std::min(exp, MAX_BACKOFF_MS);
            const qint64 jitter = static_cast<qint64>(QRandomGenerator::global()->bounded(121));
            return cap + jitter;
        }

    } // namespace

    ProviderLauncher::ProviderLauncher(NowFn nowFn, StartProcessFn startProcessFn) :
        m_nowFn(nowFn ? std::move(nowFn) : defaultNowMs), m_startProcessFn(startProcessFn ? std::move(startProcessFn) : defaultStartProcess) {}

    LaunchAttemptResult ProviderLauncher::tryLaunch(const QList<ProviderManifest>& manifests, const QString& socketPath, const QString& reason, bool hasActiveProvider,
                                                    bool hasPendingSessions, const QString& legacyFallbackPath, const QString& defaultFallbackPath, bool eager) {
        LaunchAttemptResult result;

        // On-demand launch requires a pending session (never spawn UI for nothing); eager
        // launch deliberately bypasses that to bring a resident provider up ahead of time.
        if (hasActiveProvider || (!eager && !hasPendingSessions)) {
            result.detail = QStringLiteral("skip: no launch required");
            return result;
        }

        QString    selectionError;
        const auto candidate = selectCandidate(manifests, eager, legacyFallbackPath, defaultFallbackPath, socketPath, selectionError);
        if (candidate.id.isEmpty()) {
            result.detail = selectionError;
            return result;
        }

        const qint64 nowMs = m_nowFn();

        QString      throttleReason;
        if (!canAttempt(candidate.id, nowMs, throttleReason)) {
            result.providerId = candidate.id;
            result.executable = candidate.exec;
            result.detail     = throttleReason;
            return result;
        }

        result.attempted  = true;
        result.providerId = candidate.id;
        result.executable = candidate.exec;

        const qint64 launchedPid = m_startProcessFn(candidate.exec, candidate.args, candidate.env);
        if (launchedPid <= 0) {
            markFailure(candidate.id, nowMs);
            result.detail   = QStringLiteral("launch failed for '%1' (%2)").arg(candidate.displayName, reason);
            result.launched = false;
            return result;
        }

        markSuccess(candidate.id);
        result.launched    = true;
        result.launchedPid = launchedPid;
        result.detail      = QStringLiteral("launched '%1' (%2)").arg(candidate.displayName, reason);
        return result;
    }

    qint64 ProviderLauncher::defaultNowMs() {
        return QDateTime::currentMSecsSinceEpoch();
    }

    qint64 ProviderLauncher::defaultStartProcess(const QString& program, const QStringList& args, const QProcessEnvironment& env) {
        QProcess process;
        process.setProgram(program);
        process.setArguments(args);
        process.setProcessEnvironment(env);

        qint64 pid = 0;
        if (!process.startDetached(&pid)) {
            return 0;
        }

        // startDetached blocks until the child has exec'd, so a successful launch must
        // yield a live pid. A success with no pid would strand the trust model (the
        // provider could never be attested), so treat it as a failure.
        return pid > 0 ? pid : 0;
    }

    QString ProviderLauncher::resolveExecutable(const QString& exec) {
        if (exec.contains('/')) {
            return exec;
        }

        return QStandardPaths::findExecutable(exec);
    }

    bool ProviderLauncher::isExecutableFile(const QString& path) {
        const QFileInfo info(path);
        return info.exists() && info.isFile() && info.isExecutable();
    }

    QProcessEnvironment ProviderLauncher::mergeEnvironment(const QJsonObject& envObject) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

        for (auto it = envObject.begin(); it != envObject.end(); ++it) {
            env.insert(it.key(), it.value().toString());
        }

        return env;
    }

    ProviderLauncher::SelectedCandidate ProviderLauncher::selectCandidate(const QList<ProviderManifest>& manifests, bool eager, const QString& legacyFallbackPath,
                                                                          const QString& defaultFallbackPath, const QString& socketPath, QString& selectionError) const {
        // Eager (resident) launch only ever brings up a real autostart provider; the legacy
        // env override and the built-in fallback are on-demand safety nets, not resident UI.
        const QString legacyEnvPath = legacyFallbackPath.trimmed();
        if (!eager && !legacyEnvPath.isEmpty()) {
            if (!isExecutableFile(legacyEnvPath)) {
                selectionError = QStringLiteral("skip: BB_AUTH_FALLBACK_PATH is not executable: %1").arg(legacyEnvPath);
                return SelectedCandidate{};
            }

            SelectedCandidate candidate;
            candidate.id          = LEGACY_ENV_ID;
            candidate.displayName = QStringLiteral("legacy-env");
            candidate.exec        = legacyEnvPath;
            if (!socketPath.isEmpty()) {
                candidate.args << "--socket" << socketPath;
            }
            candidate.env = QProcessEnvironment::systemEnvironment();
            return candidate;
        }

        QList<ProviderManifest> autostart;
        autostart.reserve(manifests.size());
        for (const auto& manifest : manifests) {
            if (manifest.autostart) {
                autostart.push_back(manifest);
            }
        }

        std::sort(autostart.begin(), autostart.end(), [](const ProviderManifest& a, const ProviderManifest& b) {
            if (a.priority != b.priority) {
                return a.priority > b.priority;
            }
            return a.id < b.id;
        });

        for (const auto& manifest : autostart) {
            const QString resolvedExec = resolveExecutable(manifest.exec);
            if (resolvedExec.isEmpty() || !isExecutableFile(resolvedExec)) {
                continue;
            }

            SelectedCandidate candidate;
            candidate.id          = manifest.id;
            candidate.displayName = manifest.name;
            candidate.exec        = resolvedExec;
            candidate.args        = manifest.args;
            if (!socketPath.isEmpty()) {
                candidate.args << "--socket" << socketPath;
            }
            candidate.env = mergeEnvironment(manifest.env);
            return candidate;
        }

        if (eager) {
            selectionError = QStringLiteral("skip: no autostart provider to launch eagerly");
            return SelectedCandidate{};
        }

        const QString fallbackPath = defaultFallbackPath.trimmed();
        if (fallbackPath.isEmpty() || !isExecutableFile(fallbackPath)) {
            selectionError = QStringLiteral("skip: no launchable provider candidate");
            return SelectedCandidate{};
        }

        SelectedCandidate candidate;
        candidate.id          = LEGACY_DEFAULT_ID;
        candidate.displayName = QStringLiteral("legacy-default");
        candidate.exec        = fallbackPath;
        if (!socketPath.isEmpty()) {
            candidate.args << "--socket" << socketPath;
        }
        candidate.env = QProcessEnvironment::systemEnvironment();
        return candidate;
    }

    bool ProviderLauncher::canAttempt(const QString& id, qint64 nowMs, QString& detail) const {
        const auto it = m_retryByProvider.constFind(id);
        if (it == m_retryByProvider.constEnd()) {
            return true;
        }

        if (nowMs < it->nextEligibleMs) {
            detail = QStringLiteral("skip: launch throttled until %1").arg(it->nextEligibleMs);
            return false;
        }

        return true;
    }

    void ProviderLauncher::markSuccess(const QString& id) {
        m_retryByProvider.remove(id);
    }

    void ProviderLauncher::markFailure(const QString& id, qint64 nowMs) {
        auto& state = m_retryByProvider[id];
        state.failures += 1;
        state.nextEligibleMs = nowMs + computeBackoffMs(state.failures);
    }

} // namespace bb::providers
