#pragma once

#include "RequestTypes.hpp"
#include "../RequestContext.hpp"
#include "../Session.hpp"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <utility>

namespace noctalia {

    class PinentryManager : public QObject {
        Q_OBJECT

      public:
        explicit PinentryManager(QObject* parent = nullptr);
        ~PinentryManager() override;

        // Process incoming pinentry request
        void handleRequest(const QJsonObject& msg, QLocalSocket* socket, pid_t peerPid);

        // Process response for pending user input
        struct ResponseResult {
            QJsonObject socketResponse;
        };
        ResponseResult handleResponse(const QString& cookie, const QString& response);

        // Process terminal result from pinentry mode
        QJsonObject handleResult(const QJsonObject& msg, pid_t peerPid);

        // Process cancellation
        QJsonObject handleCancel(const QString& cookie);

        // Request state
        bool          hasPendingInput(const QString& cookie) const;
        bool          hasRequest(const QString& cookie) const;
        bool          isAwaitingOutcome(const QString& cookie) const;
        QLocalSocket* getSocketForPendingInput(const QString& cookie) const;

        // Cleanup
        void cleanupForSocket(QLocalSocket* socket);

      private:
        struct AwaitingOutcome {
            PinentryRequest request;
            QTimer*         timer = nullptr;
        };

        std::pair<int, int> resolveRetryInfo(const PinentryRequest& request);
        bool                validateResultOwner(const QString& cookie, pid_t peerPid) const;

        void cleanupAwaiting(const QString& cookie);
        void closeFlow(const QString& cookie, Session::Result result, const QString& error = {});

        QHash<QString, PinentryRequest>    m_pendingRequests;
        QHash<QString, AwaitingOutcome>    m_awaitingOutcome;
        QHash<QString, PinentryRetryInfo>  m_retryInfo;
        QHash<QString, pid_t>              m_flowOwners;
        QHash<QString, QString>            m_flowKeyinfos;
        QSet<QString>                      m_retryReported;
    };

} // namespace noctalia
