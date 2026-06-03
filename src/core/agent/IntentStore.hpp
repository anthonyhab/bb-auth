#pragma once

#include <QList>
#include <QString>

#include <functional>
#include <optional>

namespace bb::agent {

    // A self-asserted declaration that a privileged command is about to run, registered
    // by a hook/MCP client BEFORE the command triggers polkit. It is display/audit only
    // and MUST NOT influence the allow/deny decision — see docs/PROVIDER_CONTRACT.md.
    struct PendingIntent {
        QString reason;
        QString declaredAgent; // agent id the declarer claimed (untrusted)
        QString command;       // command text the declarer reported (display only)
        QString cwd;           // working directory the declarer reported (display only)
        QString channel;       // "hook" | "mcp"

        // OS-resolved binding: the agent process both the declarer and the eventual
        // polkit subject descend from. startTime pins the generation so a recycled pid
        // cannot inherit a stale intent.
        qint64 agentRootPid   = 0;
        qint64 agentStartTime = 0;

        qint64 declaredAtMs = 0;
        qint64 expiresAtMs  = 0;
    };

    // Short-lived store of pending intents, correlated to polkit requests by shared agent
    // ancestry. Intents expire and are consumed on first match (one reason per command).
    class IntentStore {
      public:
        using NowFn = std::function<qint64()>;

        IntentStore();
        explicit IntentStore(NowFn nowFn);

        // Register a declared intent (declaredAtMs/expiresAtMs are stamped here).
        // ttlMs is clamped to a sane bound. Ignored when it carries no agent binding.
        void declare(PendingIntent intent, qint64 ttlMs);

        // Find and remove the freshest unexpired intent bound to (agentRootPid,
        // agentStartTime). The start-time match is the fail-closed guard against pid
        // reuse. Returns nullopt when nothing matches.
        std::optional<PendingIntent> consumeForAgent(qint64 agentRootPid, qint64 agentStartTime);

        int pruneExpired();

        [[nodiscard]] int size() const {
            return static_cast<int>(m_intents.size());
        }

      private:
        NowFn                m_nowFn;
        QList<PendingIntent> m_intents;
    };

} // namespace bb::agent
