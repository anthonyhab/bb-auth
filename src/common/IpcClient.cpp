#include "IpcClient.hpp"
#include "Constants.hpp"

#include <QJsonDocument>
#include <QLocalSocket>

namespace noctalia {

    IpcClient::IpcClient(const QString& socketPath) : m_socketPath(socketPath) {}

    std::optional<QJsonObject> IpcClient::sendRequest(const QJsonObject& request, int timeoutMs) {
        QLocalSocket socket;
        socket.connectToServer(m_socketPath);

        if (!socket.waitForConnected(IPC_CONNECT_TIMEOUT_MS))
            return std::nullopt;

        QByteArray data = QJsonDocument(request).toJson(QJsonDocument::Compact);
        data.append('\n');

        if (socket.write(data) == -1 || !socket.waitForBytesWritten(IPC_WRITE_TIMEOUT_MS))
            return std::nullopt;

        if (!socket.waitForReadyRead(timeoutMs))
            return std::nullopt;

        // Read until we get a complete line
        while (!socket.canReadLine()) {
            if (!socket.waitForReadyRead(timeoutMs))
                return std::nullopt;
        }

        const QByteArray replyLine = socket.readLine().trimmed();
        if (replyLine.isEmpty())
            return std::nullopt;

        QJsonParseError parseError;
        const auto      doc = QJsonDocument::fromJson(replyLine, &parseError);

        if (parseError.error != QJsonParseError::NoError || !doc.isObject())
            return std::nullopt;

        return doc.object();
    }

    bool IpcClient::ping() {
        auto response = sendRequest(QJsonObject{{"type", "ping"}}, IPC_READ_TIMEOUT_MS);
        return response && response->value("type").toString() == "pong";
    }

} // namespace noctalia
