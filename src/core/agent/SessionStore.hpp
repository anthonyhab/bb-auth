#pragma once

#include "../Session.hpp"

#include <memory>
#include <optional>
#include <unordered_map>

namespace bb::agent {

    class SessionStore {
      public:
        using SessionMap = std::unordered_map<QString, std::unique_ptr<bb::Session>>;

        std::optional<QJsonObject> createSession(const QString& id, Session::Source source, Session::Context ctx);
        std::optional<QJsonObject> updatePrompt(const QString& id, const QString& prompt, bool echo, bool clearError);
        std::optional<QJsonObject> updateError(const QString& id, const QString& error);
        std::optional<QJsonObject> updateInfo(const QString& id, const QString& info);
        bool                       updatePinentryRetry(const QString& id, int curRetry, int maxRetries);
        std::optional<QJsonObject> closeSession(const QString& id, Session::Result result);
        Session*                   getSession(const QString& id);
        const SessionMap&          sessions() const;
        bool                       empty() const;
        std::size_t                size() const;

      private:
        SessionMap m_sessions;
    };

} // namespace bb::agent
