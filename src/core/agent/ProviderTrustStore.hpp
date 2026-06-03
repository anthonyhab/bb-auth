#pragma once

#include <QHash>

#include <functional>

#include <sys/types.h>

namespace bb::agent {

    // Records the processes the daemon itself launched as providers, so a registration
    // can be proven to come from a daemon-launched process rather than an arbitrary
    // same-UID peer. Trust is anchored on the kernel-attested SO_PEERCRED pid plus the
    // process start-time: a pid alone is forgeable across process death + reuse, so a
    // recorded launch only matches a connecting peer whose start-time is unchanged.
    //
    // This is the trust root for the provider authorization model (see
    // docs/adr/0001-provider-trust-model.md): only a daemon-launched provider may become
    // the active provider that receives the user's secret.
    class ProviderTrustStore {
      public:
        using NowFn       = std::function<qint64()>;
        // Returns the process start-time (/proc/<pid>/stat field 22), or 0 if unreadable.
        using StartTimeFn = std::function<qint64(qint64)>;

        explicit ProviderTrustStore(NowFn nowFn = {}, StartTimeFn startTimeFn = {});

        // Record a process the daemon just launched. Reads and stores its start-time now,
        // while the process is known live (QProcess::startDetached has already blocked
        // until exec completed). A pid of <= 0 is ignored.
        void recordLaunch(qint64 pid);

        // True iff peerPid matches a recorded launch whose start-time is unchanged. The
        // match is consumed (single-use): a launch token authorizes exactly one
        // registration, so a recycled pid cannot re-use an earlier launch record.
        bool consumeTrust(qint64 peerPid);

        int  pendingCount() const;

      private:
        struct Entry {
            qint64 startTime;
            qint64 recordedAtMs;
        };

        void                  prune(qint64 nowMs);

        NowFn                 m_nowFn;
        StartTimeFn           m_startTimeFn;
        QHash<qint64, Entry>  m_launched;

        // A launched provider that never connects within this window is abandoned; the
        // record is pruned so the map cannot grow without bound on repeated relaunches.
        static constexpr qint64 TRUST_TTL_MS = 30000;
    };

} // namespace bb::agent
