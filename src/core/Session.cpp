#include "Session.hpp"

namespace bb {

    Session::Session(const QString& id, Source source, Context context) : m_id(id), m_source(source), m_context(std::move(context)) {}

    void Session::setPrompt(const QString& prompt, bool echo, bool clearError) {
        m_prompt = prompt;
        m_echo   = echo;
        m_state  = State::Prompting;
        if (clearError) {
            m_error.clear();
        }
        m_info.clear();
    }

    void Session::setError(const QString& error) {
        m_error = error;
    }

    void Session::setInfo(const QString& info) {
        m_info = info;
    }

    void Session::setPinentryRetry(int curRetry, int maxRetries) {
        if (m_source != Source::Pinentry) {
            return;
        }

        m_context.curRetry   = curRetry < 0 ? 0 : curRetry;
        m_context.maxRetries = maxRetries > 0 ? maxRetries : 3;
    }

    void Session::close(Result result) {
        m_result = result;
        m_state  = State::Closed;
        if (result == Result::Success) {
            m_error.clear();
        }
    }

    QString Session::sourceToString(Source s) {
        switch (s) {
            case Source::Polkit: return "polkit";
            case Source::Keyring: return "keyring";
            case Source::Pinentry: return "pinentry";
        }
        return "unknown";
    }

    QString Session::resultToString(Result r) {
        switch (r) {
            case Result::Success: return "success";
            case Result::Cancelled: return "cancelled";
            case Result::Error: return "error";
        }
        return "unknown";
    }

    QJsonObject Session::requestorToJson() const {
        QJsonObject obj{{"name", m_context.requestor.name}, {"icon", m_context.requestor.icon}, {"fallbackLetter", m_context.requestor.fallbackLetter}};

        if (!m_context.requestor.fallbackKey.isEmpty()) {
            obj["fallbackKey"] = m_context.requestor.fallbackKey;
        }

        if (m_context.requestor.pid > 0) {
            obj["pid"] = m_context.requestor.pid;
        }
        return obj;
    }

    QJsonObject Session::contextToJson() const {
        QJsonObject ctx{{"message", m_context.message}, {"requestor", requestorToJson()}};

        switch (m_source) {
            case Source::Polkit:
                if (!m_context.actionId.isEmpty()) {
                    ctx["actionId"] = m_context.actionId;
                }
                if (!m_context.user.isEmpty()) {
                    ctx["user"] = m_context.user;
                }
                if (!m_context.details.isEmpty()) {
                    ctx["details"] = m_context.details;
                }
                break;

            case Source::Keyring:
                if (!m_context.keyringName.isEmpty()) {
                    ctx["keyringName"] = m_context.keyringName;
                }
                break;

            case Source::Pinentry:
                if (!m_context.description.isEmpty()) {
                    ctx["description"] = m_context.description;
                }
                if (!m_context.keyinfo.isEmpty()) {
                    ctx["keyinfo"] = m_context.keyinfo;
                }
                ctx["curRetry"]    = m_context.curRetry;
                ctx["maxRetries"]  = m_context.maxRetries;
                ctx["confirmOnly"] = m_context.confirmOnly;
                ctx["repeat"]      = m_context.repeat;
                break;
        }

        return ctx;
    }

    QJsonObject Session::toCreatedEvent() const {
        return QJsonObject{{"type", "session.created"}, {"id", m_id}, {"source", sourceToString(m_source)}, {"context", contextToJson()}};
    }

    QJsonObject Session::toUpdatedEvent() const {
        QJsonObject event{{"type", "session.updated"}, {"id", m_id}, {"state", "prompting"}, {"prompt", m_prompt}, {"echo", m_echo}};

        if (m_source == Source::Pinentry) {
            event["curRetry"]   = m_context.curRetry;
            event["maxRetries"] = m_context.maxRetries;
        }

        if (!m_error.isEmpty()) {
            event["error"] = m_error;
        }

        if (!m_info.isEmpty()) {
            event["info"] = m_info;
        }

        return event;
    }

    QJsonObject Session::toClosedEvent() const {
        QJsonObject event{{"type", "session.closed"}, {"id", m_id}, {"result", resultToString(m_result.value_or(Result::Error))}};

        if (!m_error.isEmpty()) {
            event["error"] = m_error;
        }

        return event;
    }

} // namespace bb
