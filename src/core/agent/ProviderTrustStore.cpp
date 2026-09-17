#include "ProviderTrustStore.hpp"

#include "../RequestContext.hpp"

#include <QDateTime>

#include <utility>

namespace bb::agent {

    namespace {

        qint64 defaultStartTime(qint64 pid) {
            const auto proc = RequestContextHelper::readProc(pid);
            return proc ? proc->startTime : 0;
        }

    } // namespace

    ProviderTrustStore::ProviderTrustStore(NowFn nowFn, StartTimeFn startTimeFn) :
        m_nowFn(nowFn ? std::move(nowFn) : [] { return QDateTime::currentMSecsSinceEpoch(); }),
        m_startTimeFn(startTimeFn ? std::move(startTimeFn) : defaultStartTime) {}

    void ProviderTrustStore::recordLaunch(qint64 pid) {
        if (pid <= 0) {
            return;
        }

        const qint64 nowMs = m_nowFn();
        prune(nowMs);

        const qint64 startTime = m_startTimeFn(pid);
        if (startTime <= 0) {
            // Without a start-time the record cannot defend against pid reuse, so it is
            // not trustworthy. Drop it: the provider will be denied and relaunched.
            return;
        }

        m_launched[pid] = Entry{startTime, nowMs};
    }

    // @lat: [[provider-trust#Daemon-launch attestation]]
    bool ProviderTrustStore::consumeTrust(qint64 peerPid) {
        if (peerPid <= 0) {
            return false;
        }

        const qint64 nowMs = m_nowFn();
        prune(nowMs);

        const auto it = m_launched.constFind(peerPid);
        if (it == m_launched.constEnd()) {
            return false;
        }

        // The connecting peer is alive (the socket is connected). If its start-time still
        // matches the launch record it is the very process the daemon launched; if it
        // differs, the original died and this pid was recycled by another process.
        const bool matches = m_startTimeFn(peerPid) == it->startTime;
        m_launched.erase(it);
        return matches;
    }

    int ProviderTrustStore::pendingCount() const {
        return static_cast<int>(m_launched.size());
    }

    void ProviderTrustStore::prune(qint64 nowMs) {
        for (auto it = m_launched.begin(); it != m_launched.end();) {
            if ((nowMs - it->recordedAtMs) > TRUST_TTL_MS) {
                it = m_launched.erase(it);
            } else {
                ++it;
            }
        }
    }

} // namespace bb::agent
