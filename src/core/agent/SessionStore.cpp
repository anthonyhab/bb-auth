#include "SessionStore.hpp"

namespace bb::agent {

    std::optional<QJsonObject> SessionStore::createSession(const QString& id, Session::Source source, Session::Context ctx) {
        if (m_sessions.find(id) != m_sessions.end()) {
            return std::nullopt;
        }

        auto session      = std::make_unique<bb::Session>(id, source, ctx);
        auto createdEvent = session->toCreatedEvent();
        m_sessions[id]    = std::move(session);
        return createdEvent;
    }

    std::optional<QJsonObject> SessionStore::updatePrompt(const QString& id, const QString& prompt, bool echo, bool clearError) {
        auto it = m_sessions.find(id);
        if (it == m_sessions.end()) {
            return std::nullopt;
        }

        it->second->setPrompt(prompt, echo, clearError);
        return it->second->toUpdatedEvent();
    }

    std::optional<QJsonObject> SessionStore::updateError(const QString& id, const QString& error) {
        auto it = m_sessions.find(id);
        if (it == m_sessions.end()) {
            return std::nullopt;
        }

        it->second->setError(error);
        return it->second->toUpdatedEvent();
    }

    std::optional<QJsonObject> SessionStore::updateInfo(const QString& id, const QString& info) {
        auto it = m_sessions.find(id);
        if (it == m_sessions.end()) {
            return std::nullopt;
        }

        it->second->setInfo(info);
        return it->second->toUpdatedEvent();
    }

    bool SessionStore::updatePinentryRetry(const QString& id, int curRetry, int maxRetries) {
        auto it = m_sessions.find(id);
        if (it == m_sessions.end() || it->second->source() != Session::Source::Pinentry) {
            return false;
        }

        it->second->setPinentryRetry(curRetry, maxRetries);
        return true;
    }

    std::optional<QJsonObject> SessionStore::closeSession(const QString& id, Session::Result result) {
        auto it = m_sessions.find(id);
        if (it == m_sessions.end()) {
            return std::nullopt;
        }

        it->second->close(result);
        auto event = it->second->toClosedEvent();
        m_sessions.erase(it);
        return event;
    }

    Session* SessionStore::getSession(const QString& id) {
        auto it = m_sessions.find(id);
        return (it != m_sessions.end()) ? it->second.get() : nullptr;
    }

    const SessionStore::SessionMap& SessionStore::sessions() const {
        return m_sessions;
    }

    bool SessionStore::empty() const {
        return m_sessions.empty();
    }

    std::size_t SessionStore::size() const {
        return m_sessions.size();
    }

} // namespace bb::agent
