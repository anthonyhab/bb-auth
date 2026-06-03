#include "IntentStore.hpp"

#include <QDateTime>

namespace bb::agent {

    namespace {
        constexpr qint64 MAX_TTL_MS     = 60 * 1000; // an intent older than this is never trusted
        constexpr qint64 DEFAULT_TTL_MS = 20 * 1000;
        constexpr int    MAX_INTENTS    = 64; // backstop against unbounded growth
    } // namespace

    IntentStore::IntentStore() : m_nowFn([]() { return QDateTime::currentMSecsSinceEpoch(); }) {}

    IntentStore::IntentStore(NowFn nowFn) : m_nowFn(std::move(nowFn)) {}

    void IntentStore::declare(PendingIntent intent, qint64 ttlMs) {
        // An intent with no agent binding can never be correlated to a subject — drop it
        // rather than let it linger and risk a loose fallback match.
        if (intent.agentRootPid <= 0) {
            return;
        }

        if (ttlMs <= 0 || ttlMs > MAX_TTL_MS) {
            ttlMs = DEFAULT_TTL_MS;
        }

        const qint64 now    = m_nowFn();
        intent.declaredAtMs = now;
        intent.expiresAtMs  = now + ttlMs;

        // Replace any prior intent for the same agent generation: the latest declaration
        // wins (an agent issuing a new command supersedes a stale one it never used).
        for (auto it = m_intents.begin(); it != m_intents.end();) {
            if (it->agentRootPid == intent.agentRootPid && it->agentStartTime == intent.agentStartTime) {
                it = m_intents.erase(it);
            } else {
                ++it;
            }
        }

        m_intents.append(std::move(intent));
        pruneExpired();

        // Hard cap: drop the oldest if we somehow exceed the backstop.
        while (m_intents.size() > MAX_INTENTS) {
            m_intents.removeFirst();
        }
    }

    std::optional<PendingIntent> IntentStore::consumeForAgent(qint64 agentRootPid, qint64 agentStartTime) {
        if (agentRootPid <= 0) {
            return std::nullopt;
        }

        pruneExpired();

        int    bestIdx        = -1;
        qint64 bestDeclaredAt = -1;
        for (int i = 0; i < m_intents.size(); ++i) {
            const auto& intent = m_intents[i];
            if (intent.agentRootPid != agentRootPid) {
                continue;
            }
            // Fail-closed reuse guard: same pid but a different generation is NOT a match.
            if (intent.agentStartTime != agentStartTime) {
                continue;
            }
            if (intent.declaredAtMs > bestDeclaredAt) {
                bestDeclaredAt = intent.declaredAtMs;
                bestIdx        = i;
            }
        }

        if (bestIdx < 0) {
            return std::nullopt;
        }

        const PendingIntent match = m_intents.takeAt(bestIdx);
        return match;
    }

    int IntentStore::pruneExpired() {
        const qint64 now     = m_nowFn();
        int          removed = 0;
        for (auto it = m_intents.begin(); it != m_intents.end();) {
            if (it->expiresAtMs <= now) {
                it = m_intents.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }

} // namespace bb::agent
