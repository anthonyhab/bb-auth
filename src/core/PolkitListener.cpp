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
    for (auto* state : sessions.values()) {
        delete state;
    }
}

void CPolkitListener::initiateAuthentication(const QString& actionId, const QString& message, const QString& iconName, const PolkitQt1::Details& details, const QString& cookie,
                                             const PolkitQt1::Identity::List& identities, AsyncResult* result) {

    std::print("> New authentication session (cookie: {})\n", cookie.toStdString());

    if (sessions.contains(cookie)) {
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

    sessions.insert(cookie, state);

    g_pAgent->initAuthPrompt(cookie);

    reattempt(state);
}

void CPolkitListener::reattempt(SessionState* state) {
    state->cancelled = false;

    state->session = new Session(state->selectedUser, state->cookie, state->result);
    connect(state->session, SIGNAL(request(QString, bool)), this, SLOT(request(QString, bool)));
    connect(state->session, SIGNAL(completed(bool)), this, SLOT(completed(bool)));
    connect(state->session, SIGNAL(showError(QString)), this, SLOT(showError(QString)));
    connect(state->session, SIGNAL(showInfo(QString)), this, SLOT(showInfo(QString)));

    state->session->initiate();
}

bool CPolkitListener::initiateAuthenticationFinish() {
    std::print("> initiateAuthenticationFinish()\n");
    return true;
}

void CPolkitListener::cancelAuthentication() {
    std::print("> cancelAuthentication() - cancelling ALL sessions\n");

    for (auto* state : sessions.values()) {
        state->cancelled = true;
        finishAuth(state);
    }
}

CPolkitListener::SessionState* CPolkitListener::findStateForSession(Session* session) {
    for (auto* state : sessions.values()) {
        if (state->session == session)
            return state;
    }
    return nullptr;
}

void CPolkitListener::request(const QString& request, bool echo) {
    auto* session = qobject_cast<Session*>(sender());
    auto* state   = findStateForSession(session);
    if (!state)
        return;

    std::print("> PKS request (cookie: {}): {} echo: {}\n", state->cookie.toStdString(), request.toStdString(), echo);
    state->prompt = request;
    state->echoOn = echo;

    state->requestSent = true;
    g_pAgent->enqueueRequest(state->cookie);
}

void CPolkitListener::completed(bool gainedAuthorization) {
    auto* session = qobject_cast<Session*>(sender());
    auto* state   = findStateForSession(session);
    if (!state)
        return;

    std::print("> PKS completed (cookie: {}): {}\n", state->cookie.toStdString(), gainedAuthorization ? "Auth successful" : "Auth unsuccessful");

    state->gainedAuth = gainedAuthorization;

    if (!gainedAuthorization) {
        state->errorText = "Authentication failed";
        g_pAgent->enqueueError(state->cookie, state->errorText);
    }

    finishAuth(state);
}

void CPolkitListener::showError(const QString& text) {
    auto* session = qobject_cast<Session*>(sender());
    auto* state   = findStateForSession(session);
    if (!state)
        return;

    std::print("> PKS showError (cookie: {}): {}\n", state->cookie.toStdString(), text.toStdString());

    state->errorText = text;
    g_pAgent->enqueueError(state->cookie, text);
}

void CPolkitListener::showInfo(const QString& text) {
    auto* session = qobject_cast<Session*>(sender());
    auto* state   = findStateForSession(session);
    if (!state)
        return;

    std::print("> PKS showInfo (cookie: {}): {}\n", state->cookie.toStdString(), text.toStdString());
}

void CPolkitListener::finishAuth(SessionState* state) {
    if (!state->inProgress) {
        std::print("> finishAuth: ODD. !state->inProgress for cookie {}\n", state->cookie.toStdString());
        return;
    }

    if (!state->gainedAuth && !state->cancelled) {
        state->retryCount++;
        if (state->retryCount < SessionState::MAX_AUTH_RETRIES) {
            std::print("> finishAuth: Did not gain auth (attempt {}/{}). Reattempting for cookie {}.\n", state->retryCount, SessionState::MAX_AUTH_RETRIES,
                       state->cookie.toStdString());
            state->session->deleteLater();
            reattempt(state);
            return;
        } else {
            std::print("> finishAuth: Max retries ({}) reached for cookie {}. Failing.\n", SessionState::MAX_AUTH_RETRIES, state->cookie.toStdString());
            state->errorText = "Too many failed attempts";
            g_pAgent->enqueueError(state->cookie, state->errorText);
        }
    }

    std::print("> finishAuth: Gained auth, cancelled, or max retries reached. Cleaning up cookie {}.\n", state->cookie.toStdString());

    state->inProgress = false;

    if (state->session) {
        state->session->result()->setCompleted();
        state->session->deleteLater();
    } else
        state->result->setCompleted();

    if (state->gainedAuth)
        g_pAgent->enqueueComplete(state->cookie, "success");
    else if (state->cancelled)
        g_pAgent->enqueueComplete(state->cookie, "cancelled");

    sessions.remove(state->cookie);
    delete state;
}

void CPolkitListener::submitPassword(const QString& cookie, const QString& pass) {
    if (!sessions.contains(cookie))
        return;

    auto* state = sessions[cookie];
    if (!state->session)
        return;

    state->session->setResponse(pass);
}

void CPolkitListener::cancelPending(const QString& cookie) {
    if (!sessions.contains(cookie))
        return;

    auto* state = sessions[cookie];
    if (!state->session)
        return;

    state->session->cancel();

    state->cancelled = true;

    finishAuth(state);
}
