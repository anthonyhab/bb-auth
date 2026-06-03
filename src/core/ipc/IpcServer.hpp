#pragma once

#include "../../common/Constants.hpp"

#include <QHash>
#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

#include <functional>

namespace bb {

    // Callback type for handling parsed JSON messages
    // Parameters: socket, message type, full JSON object
    using MessageHandler = std::function<void(QLocalSocket*, const QString&, const QJsonObject&)>;

    class IpcServer : public QObject {
        Q_OBJECT

      public:
        explicit IpcServer(QObject* parent = nullptr);
        ~IpcServer() override;

        // Start listening on the given socket path
        // Returns false if binding fails
        bool start(const QString& socketPath);

        // Stop the server and disconnect all clients
        void stop();

        // Set the handler for incoming messages
        void setMessageHandler(MessageHandler handler);

        // Send a JSON response to a specific socket
        // If secureWipe is true, zeros the buffer after sending
        void sendJson(QLocalSocket* socket, const QJsonObject& json, bool secureWipe = false);

        // Get peer process ID for a connected socket
        // Returns -1 on failure
        static pid_t getPeerPid(QLocalSocket* socket);

        // Override the incomplete-frame timeout (ms). A client that buffers a partial
        // frame (no terminating newline) for longer than this is disconnected. Mainly for
        // tests; defaults to INCOMPLETE_FRAME_TIMEOUT_MS.
        void         setIncompleteFrameTimeoutMs(int ms);

      Q_SIGNALS:
        void clientConnected(QLocalSocket* socket);
        void clientDisconnected(QLocalSocket* socket);

      private Q_SLOTS:
        void onNewConnection();
        void onReadyRead();
        void onDisconnected();
        void sweepIncompleteFrames();

      private:
        void                             handleLine(QLocalSocket* socket, const QByteArray& line);
        void                             trackIncompleteFrame(QLocalSocket* socket, bool hasPartialFrame);

        QLocalServer*                    m_server = nullptr;
        MessageHandler                   m_handler;
        QHash<QLocalSocket*, QByteArray> m_buffers;
        // Time (ms since epoch) a socket's still-incomplete frame first appeared. Absent
        // once the buffer drains to empty. Drives the slow-drip disconnect sweep (F6).
        QHash<QLocalSocket*, qint64>     m_incompleteFrameSince;
        QTimer                           m_idleSweepTimer;
        int                              m_incompleteFrameTimeoutMs = INCOMPLETE_FRAME_TIMEOUT_MS;
    };

} // namespace bb
