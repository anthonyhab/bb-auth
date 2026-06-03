#include "IpcServer.hpp"
#include "../../common/Constants.hpp"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>

#include <algorithm>

#include <sys/socket.h>
#include <cstring>

namespace bb {

    namespace {

        void secureZero(void* ptr, std::size_t size) {
            volatile unsigned char* p = static_cast<volatile unsigned char*>(ptr);
            while (size--) {
                *p++ = 0;
            }
        }

    } // namespace

    namespace {

        // Sweep often enough to honour the timeout without busy-spinning: half the timeout,
        // bounded so a tiny test timeout still fires promptly and production stays cheap.
        int sweepIntervalForTimeout(int timeoutMs) {
            return std::clamp(timeoutMs / 2, 20, 2000);
        }

    } // namespace

    IpcServer::IpcServer(QObject* parent) : QObject(parent) {
        m_idleSweepTimer.setInterval(sweepIntervalForTimeout(m_incompleteFrameTimeoutMs));
        connect(&m_idleSweepTimer, &QTimer::timeout, this, &IpcServer::sweepIncompleteFrames);
    }

    IpcServer::~IpcServer() {
        stop();
    }

    bool IpcServer::start(const QString& socketPath) {
        if (m_server)
            return false;

        // Remove stale socket file
        if (QFile::exists(socketPath)) {
            QFile::remove(socketPath);
        }

        m_server = new QLocalServer(this);
        m_server->setSocketOptions(QLocalServer::UserAccessOption);

        if (!m_server->listen(socketPath)) {
            delete m_server;
            m_server = nullptr;
            return false;
        }

        connect(m_server, &QLocalServer::newConnection, this, &IpcServer::onNewConnection);
        m_idleSweepTimer.start();
        return true;
    }

    void IpcServer::setIncompleteFrameTimeoutMs(int ms) {
        m_incompleteFrameTimeoutMs = ms;
        m_idleSweepTimer.setInterval(sweepIntervalForTimeout(ms));
    }

    void IpcServer::stop() {
        if (!m_server)
            return;

        m_idleSweepTimer.stop();

        // Disconnect all clients
        for (auto* socket : m_buffers.keys()) {
            socket->disconnectFromServer();
        }
        m_buffers.clear();
        m_incompleteFrameSince.clear();

        m_server->close();
        delete m_server;
        m_server = nullptr;
    }

    void IpcServer::setMessageHandler(MessageHandler handler) {
        m_handler = std::move(handler);
    }

    void IpcServer::sendJson(QLocalSocket* socket, const QJsonObject& json, bool secureWipe) {
        if (!socket || socket->state() != QLocalSocket::ConnectedState)
            return;

        QByteArray data = QJsonDocument(json).toJson(QJsonDocument::Compact);
        data.append('\n');

        socket->write(data);
        socket->flush();

        if (secureWipe) {
            secureZero(data.data(), static_cast<std::size_t>(data.size()));
        }
    }

    pid_t IpcServer::getPeerPid(QLocalSocket* socket) {
        if (!socket)
            return -1;

        struct ucred cred;
        socklen_t    len = sizeof(cred);

        const int    fd = static_cast<int>(socket->socketDescriptor());
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == -1) {
            return -1;
        }

        return cred.pid;
    }

    void IpcServer::onNewConnection() {
        while (m_server->hasPendingConnections()) {
            QLocalSocket* socket = m_server->nextPendingConnection();
            if (!socket)
                continue;

            m_buffers[socket] = QByteArray();

            connect(socket, &QLocalSocket::readyRead, this, &IpcServer::onReadyRead);
            connect(socket, &QLocalSocket::disconnected, this, &IpcServer::onDisconnected);

            emit clientConnected(socket);
        }
    }

    void IpcServer::onReadyRead() {
        auto* socket = qobject_cast<QLocalSocket*>(sender());
        if (!socket)
            return;

        QByteArray& buffer = m_buffers[socket];
        buffer.append(socket->readAll());

        // Enforce max message size
        if (buffer.size() > static_cast<qsizetype>(MAX_MESSAGE_SIZE)) {
            socket->disconnectFromServer();
            return;
        }

        // Process complete lines
        qsizetype idx;
        while ((idx = buffer.indexOf('\n')) != -1) {
            QByteArray line = buffer.left(idx).trimmed();
            buffer.remove(0, static_cast<qsizetype>(idx + 1));

            if (!line.isEmpty()) {
                handleLine(socket, line);
            }
        }

        // Whatever remains is an incomplete frame. Start (but never reset) its clock so a
        // peer that drips bytes without ever sending a newline still times out (F6).
        trackIncompleteFrame(socket, !buffer.isEmpty());
    }

    void IpcServer::trackIncompleteFrame(QLocalSocket* socket, bool hasPartialFrame) {
        if (!hasPartialFrame) {
            m_incompleteFrameSince.remove(socket);
        } else if (!m_incompleteFrameSince.contains(socket)) {
            m_incompleteFrameSince.insert(socket, QDateTime::currentMSecsSinceEpoch());
        }
    }

    void IpcServer::sweepIncompleteFrames() {
        const qint64         now = QDateTime::currentMSecsSinceEpoch();

        QList<QLocalSocket*> stale;
        for (auto it = m_incompleteFrameSince.constBegin(); it != m_incompleteFrameSince.constEnd(); ++it) {
            if ((now - it.value()) > m_incompleteFrameTimeoutMs) {
                stale.append(it.key());
            }
        }

        for (QLocalSocket* socket : stale) {
            m_incompleteFrameSince.remove(socket);
            if (socket) {
                socket->disconnectFromServer();
            }
        }
    }

    void IpcServer::onDisconnected() {
        auto* socket = qobject_cast<QLocalSocket*>(sender());
        if (!socket)
            return;

        m_buffers.remove(socket);
        m_incompleteFrameSince.remove(socket);
        emit clientDisconnected(socket);

        socket->deleteLater();
    }

    void IpcServer::handleLine(QLocalSocket* socket, const QByteArray& line) {
        if (!m_handler)
            return;

        QJsonParseError parseError;
        const auto      doc = QJsonDocument::fromJson(line, &parseError);

        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            sendJson(socket, QJsonObject{{"type", "error"}, {"message", "Invalid JSON"}});
            return;
        }

        const QJsonObject obj  = doc.object();
        const QString     type = obj.value("type").toString();

        if (type.isEmpty()) {
            sendJson(socket, QJsonObject{{"type", "error"}, {"message", "Missing type field"}});
            return;
        }

        m_handler(socket, type, obj);
    }

} // namespace bb
