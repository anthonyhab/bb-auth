#pragma once

#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <optional>

namespace bb {

    class Session {
      public:
        enum class Source {
            Polkit,
            Keyring,
            Pinentry
        };
        enum class State {
            Prompting,
            Closed
        };
        enum class Result {
            Success,
            Cancelled,
            Error
        };

        struct Requestor {
            QString name;
            QString icon;
            QString fallbackLetter;
            QString fallbackKey;
            qint64  pid{0};
            // OS-resolved identity — the trust anchor. Set when the resolved process
            // is recognized as an AI agent runtime (see RequestContext agent registry).
            bool    isAgent{false};
            QString agentKind; // e.g. "claude-code" (empty when not an agent)
        };

        // Declared intent from a privileged-command actor (hook or MCP). This is
        // self-asserted and untrusted: it is surfaced for human display/audit ONLY and
        // MUST NOT influence the allow/deny decision. Kept separate from Requestor so
        // the wire format keeps OS-truth and declaration legibly distinct.
        struct Intent {
            QString reason;        // why the command is being run
            QString declaredAgent; // agent id the declarer claimed (untrusted)
            QString channel;       // "hook" | "mcp"
            bool    mismatch{false}; // declaredAgent disagrees with OS-resolved agentKind

            [[nodiscard]] bool isValid() const {
                return !reason.isEmpty() || !declaredAgent.isEmpty();
            }
        };

        struct Context {
            // Common fields
            QString   message;
            Requestor requestor;
            Intent    intent;

            // Polkit-specific
            QString     actionId;
            QString     user;
            QJsonObject details;

            // Keyring-specific
            QString keyringName;

            // Pinentry-specific
            QString description;
            QString keyinfo;
            int     curRetry{0};
            int     maxRetries{3};
            bool    confirmOnly{false};
            bool    repeat{false};
        };

        // Construction
        Session(const QString& id, Source source, Context context);

        // Accessors
        [[nodiscard]] QString id() const {
            return m_id;
        }
        [[nodiscard]] Source source() const {
            return m_source;
        }
        [[nodiscard]] State state() const {
            return m_state;
        }
        [[nodiscard]] qint64 createdAtMs() const {
            return m_createdAtMs;
        }

        // State transitions
        void setPrompt(const QString& prompt, bool echo = false, bool clearError = true);
        void setError(const QString& error);
        void setInfo(const QString& info);
        void setPinentryRetry(int curRetry, int maxRetries);
        void close(Result result);

        // Serialization (v2 protocol)
        [[nodiscard]] QJsonObject toCreatedEvent() const;
        [[nodiscard]] QJsonObject toUpdatedEvent() const;
        [[nodiscard]] QJsonObject toClosedEvent() const;

      private:
        QString                      m_id;
        Source                       m_source;
        Context                      m_context;
        qint64                       m_createdAtMs{0};
        State                        m_state{State::Prompting};
        QString                      m_prompt;
        QString                      m_error;
        QString                      m_info;
        bool                         m_echo{false};
        std::optional<Result>        m_result;
        [[nodiscard]] static QString sourceToString(Source s);
        [[nodiscard]] static QString resultToString(Result r);
        [[nodiscard]] QJsonObject    requestorToJson() const;
        [[nodiscard]] QJsonObject    intentToJson() const;
        [[nodiscard]] QJsonObject    contextToJson() const;
    };

} // namespace bb
