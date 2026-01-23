#pragma once

#include <QObject>
#include <QString>
#include <QHash>

#include <polkitqt1-agent-listener.h>
#include <polkitqt1-identity.h>
#include <polkitqt1-details.h>
#include <polkitqt1-agent-session.h>

namespace noctalia {
    class CAgent;
}

class CPolkitListener : public PolkitQt1::Agent::Listener {
    Q_OBJECT
    Q_DISABLE_COPY(CPolkitListener)

  public:
    CPolkitListener(QObject* parent = nullptr);
    ~CPolkitListener() override;

    void submitPassword(const QString& cookie, const QString& pass);
    void cancelPending(const QString& cookie);

  Q_SIGNALS:
    // Signal removed, CAgent handles logic now
    void completed(bool gainedAuthorization);

  public Q_SLOTS:
    void initiateAuthentication(const QString& actionId, const QString& message, const QString& iconName, const PolkitQt1::Details& details, const QString& cookie,
                                const PolkitQt1::Identity::List& identities, PolkitQt1::Agent::AsyncResult* result) override;
    bool initiateAuthenticationFinish() override;
    void cancelAuthentication() override;

    void onSessionRequest(const QString& request, bool echo);
    void onSessionCompleted(bool gainedAuthorization);
    void onSessionError(const QString& text);
    void onSessionInfo(const QString& text);

  private:
    struct SessionState {
        bool                           inProgress = false, cancelled = false, gainedAuth = false;
        QString                        cookie, message, iconName, actionId;
        QString                        prompt, errorText;
        bool                           echoOn      = false;
        bool                           requestSent = false;
        PolkitQt1::Details             details;
        PolkitQt1::Agent::AsyncResult* result = nullptr;
        PolkitQt1::Identity            selectedUser;
        PolkitQt1::Agent::Session*     session = nullptr;

        int                            retryCount       = 0;
        static constexpr int           MAX_AUTH_RETRIES = 3;
    };

    // Map from cookie to session state
    QHash<QString, SessionState*> m_cookieToState;
    // Reverse map for O(1) lookup
    QHash<PolkitQt1::Agent::Session*, SessionState*> m_sessionToState;

    void                                             reattempt(SessionState* state);
    void                                             finishAuth(SessionState* state);
    SessionState*                                    findStateForSession(PolkitQt1::Agent::Session* session);

    friend class noctalia::CAgent;
};
