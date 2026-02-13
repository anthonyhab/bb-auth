#include <QDebug>
#include "PolkitListener.hpp"
#include "Agent.hpp"
#include <polkitqt1-agent-session.h>

#include <print>

#include "RequestContext.hpp"

using namespace PolkitQt1::Agent;

CPolkitListener::CPolkitListener(QObject* parent) : Listener(parent) {
    ;
}

CPolkitListener::~CPolkitListener() {
    for (auto* state : m_cookieToState.values()) {
        delete state;
    }
}

void CPolkitListener::initiateAuthentication(const QString& actionId, const QString& message, const QString& iconName, const PolkitQt1::Details& details, const QString& cookie,
                                             const PolkitQt1::Identity::List& identities, AsyncResult* result) {

    std::print("> New authentication session (cookie: {})\n", cookie.toStdString());

    if (m_cookieToState.contains(cookie)) {
        std::print("> REJECTING: Session with cookie {} already exists\n", cookie.toStdString());
        result->setError("Duplicate session");
        result->setCompleted();
        return;
    }

    if (identities.isEmpty()) {
        result->setError("No identities, this is a problem with your system configuration.");
        result->setCompleted();
        std::print("> REJECTING: No idents\n");
        return;
    }

    auto* state         = new SessionState;
    state->selectedUser = identities.at(0);
    state->cookie       = cookie;
    state->result       = result;
    state->actionId     = actionId;
    state->message      = message;
    state->iconName     = iconName;
    state->gainedAuth   = false;
    state->cancelled    = false;
    state->prompt       = "";
    state->errorText    = "";
    state->echoOn       = false;
    state->requestSent  = false;
    state->details      = details;
    state->inProgress   = true;

    state->session = new PolkitQt1::Agent::Session(state->selectedUser, state->cookie, state->result);
    m_cookieToState.insert(cookie, state);
    m_sessionToState.insert(state->session, state);

    g_pAgent->onPolkitRequest(cookie, message, iconName, actionId, state->selectedUser.toString(), details);

    reattempt(state);
}

void CPolkitListener::reattempt(SessionState* state) {
    state->cancelled = false;

    // We can't guarantee that the request will be valid when we return,
    // so we just start the authentication.
    connect(state->session, &PolkitQt1::Agent::Session::request, this, &CPolkitListener::onSessionRequest);
    connect(state->session, &PolkitQt1::Agent::Session::completed, this, &CPolkitListener::onSessionCompleted);
    connect(state->session, &PolkitQt1::Agent::Session::showError, this, &CPolkitListener::onSessionError);
    connect(state->session, &PolkitQt1::Agent::Session::showInfo, this, &CPolkitListener::onSessionInfo);

    state->session->initiate();
}

bool CPolkitListener::initiateAuthenticationFinish() {
    std::print("> initiateAuthenticationFinish()\n");
    return true;
}

void CPolkitListener::cancelAuthentication() {
    std::print("> cancelAuthentication() - cancelling ALL sessions\n");

    for (auto* state : m_cookieToState.values()) {
        state->cancelled = true;
        finishAuth(state);
    }
}

CPolkitListener::SessionState* CPolkitListener::findStateForSession(PolkitQt1::Agent::Session* session) {
    return m_sessionToState.value(session, nullptr);
}

void CPolkitListener::onSessionRequest(const QString& request, bool echo) {
    auto* session = qobject_cast<Session*>(sender());
    auto* state   = findStateForSession(session);
    if (!state)
        return;

    std::print("> PKS request (cookie: {}): {} echo: {}\n", state->cookie.toStdString(), request.toStdString(), echo);
    state->prompt = request;
    state->echoOn = echo;

    state->requestSent = true;
    g_pAgent->onSessionRequest(state->cookie, request, echo);
}

void CPolkitListener::onSessionCompleted(bool gainedAuthorization) {
    auto* session = qobject_cast<Session*>(sender());
    auto* state   = findStateForSession(session);
    if (!state)
        return;

    std::print("> PKS completed (cookie: {}): {}\n", state->cookie.toStdString(), gainedAuthorization ? "Auth successful" : "Auth unsuccessful");

    state->gainedAuth = gainedAuthorization;

    if (!gainedAuthorization) {
        state->errorText = "Authentication failed";
        g_pAgent->onSessionRetry(state->cookie, state->errorText);
    }

    finishAuth(state);
}

void CPolkitListener::onSessionError(const QString& text) {
    auto* session = qobject_cast<Session*>(sender());
    auto* state   = findStateForSession(session);
    if (!state)
        return;

    std::print("> PKS showError (cookie: {}): {}\n", state->cookie.toStdString(), text.toStdString());

    state->errorText = text;
    g_pAgent->onSessionRetry(state->cookie, text);
}

void CPolkitListener::onSessionInfo(const QString& text) {
    auto* session = qobject_cast<Session*>(sender());
    auto* state   = findStateForSession(session);
    if (!state)
        return;

    std::print("> PKS showInfo (cookie: {}): {}\n", state->cookie.toStdString(), text.toStdString());
    g_pAgent->onSessionInfo(state->cookie, text);
}

void CPolkitListener::finishAuth(SessionState* state) {
    if (!state)
        return;

    if (!state->inProgress) {
        std::print("> finishAuth: ODD. !state->inProgress for cookie {}\n", state->cookie.toStdString());
        return;
    }

    if (!state->gainedAuth && !state->cancelled) {
        state->retryCount++;
        if (state->retryCount < SessionState::MAX_AUTH_RETRIES) {
            std::print("> finishAuth: Did not gain auth (attempt {}/{}). Reattempting for cookie {}.\n", state->retryCount, SessionState::MAX_AUTH_RETRIES,
                       state->cookie.toStdString());

            // Clean up old session but keep state
            if (state->session) {
                m_sessionToState.remove(state->session);
                state->session->deleteLater();
            }

            // Create new session
            state->session = new PolkitQt1::Agent::Session(state->selectedUser, state->cookie, state->result);
            m_sessionToState.insert(state->session, state);

            reattempt(state);
            return;
        } else {
            std::print("> finishAuth: Max retries ({}) reached for cookie {}. Failing.\n", SessionState::MAX_AUTH_RETRIES, state->cookie.toStdString());
            state->errorText = "Too many failed attempts";
            g_pAgent->onSessionRetry(state->cookie, state->errorText);
        }
    }

    std::print("> finishAuth: Gained auth, cancelled, or max retries reached. Cleaning up cookie {}.\n", state->cookie.toStdString());

    state->inProgress = false;

    if (state->session) {
        state->session->result()->setCompleted();
        m_sessionToState.remove(state->session);
        state->session->deleteLater();
    } else
        state->result->setCompleted();

    g_pAgent->onSessionComplete(state->cookie, state->gainedAuth);

    emit completed(state->gainedAuth);

    m_cookieToState.remove(state->cookie);
    delete state;
}

void CPolkitListener::submitPassword(const QString& cookie, const QString& pass) {
    if (!m_cookieToState.contains(cookie))
        return;

    auto* state = m_cookieToState[cookie];
    if (!state->session)
        return;

    state->session->setResponse(pass);
}

void CPolkitListener::cancelPending(const QString& cookie) {
    if (!m_cookieToState.contains(cookie))
        return;

    auto* state = m_cookieToState[cookie];
    if (!state->session)
        return;

    state->session->cancel();

    state->cancelled = true;

    finishAuth(state);
}
