#pragma once

#include <QJsonObject>
#include <QString>

#include <optional>

namespace noctalia {

    // Unified IPC client for communicating with the daemon
    class IpcClient {
      public:
        explicit IpcClient(const QString& socketPath);

        // Send a JSON request and wait for response
        // Returns std::nullopt on connection/timeout/parse failure
        std::optional<QJsonObject> sendRequest(const QJsonObject& request,
                                               int                timeoutMs = 5 * 60 * 1000 // Default 5 minutes for pinentry
        );

        // Quick ping to check if daemon is reachable
        bool ping();

      private:
        QString m_socketPath;
    };

} // namespace noctalia
