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
        };

        struct Context {
            // Common fields
            QString   message;
            Requestor requestor;

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
        State                        m_state{State::Prompting};
        QString                      m_prompt;
        QString                      m_error;
        QString                      m_info;
        bool                         m_echo{false};
        std::optional<Result>        m_result;
        [[nodiscard]] static QString sourceToString(Source s);
        [[nodiscard]] static QString resultToString(Result r);
        [[nodiscard]] QJsonObject    requestorToJson() const;
        [[nodiscard]] QJsonObject    contextToJson() const;
    };

} // namespace bb
