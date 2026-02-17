#include "KeyringManager.hpp"
#include "../Agent.hpp"

#include <QJsonDocument>
#include <QUuid>

namespace bb {

    KeyringManager::KeyringManager(QObject* parent) : QObject(parent) {}

    void KeyringManager::handleRequest(const QJsonObject& msg, QLocalSocket* socket, pid_t peerPid) {
        QString cookie = msg.value("cookie").toString();
        if (cookie.isEmpty()) {
            cookie = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }

        KeyringRequest request;
        request.cookie  = cookie;
        request.socket  = socket;
        request.peerPid = peerPid;

        if (msg.contains("title")) {
            request.title = msg.value("title").toString();
        } else {
            request.title = msg.value("prompt").toString();
        }

        request.message = msg.value("message").toString();
        request.choice  = msg.value("choice").toString();
        request.flags   = msg.value("flags").toInt();

        m_pendingRequests[cookie] = request;

        // Resolve requestor
        std::optional<ProcInfo> proc = RequestContextHelper::readProc(peerPid);
        ActorInfo               actor;
        if (proc) {
            actor = RequestContextHelper::resolveRequestorFromSubject(*proc, getuid());
        }

        bb::Session::Context ctx;
        ctx.message = request.title;
        ctx.keyringName = request.message; // Detailed message
        ctx.requestor.name = actor.displayName;
        ctx.requestor.icon = actor.iconName;
        ctx.requestor.fallbackLetter = actor.fallbackLetter;
        ctx.requestor.fallbackKey = actor.fallbackKey;
        ctx.requestor.pid = peerPid;

        // Use centralized session management
        if (!g_pAgent->createSession(cookie, bb::Session::Source::Keyring, ctx)) {
            m_pendingRequests.remove(cookie);

            QJsonObject error{{"type", "error"}, {"message", "Session ID collision"}};
            QJsonDocument doc(error);
            if (socket && socket->isOpen()) {
                socket->write(doc.toJson(QJsonDocument::Compact) + "\n");
                socket->flush();
            }
            return;
        }
        g_pAgent->updateSessionPrompt(cookie, request.message, false);
    }

    QJsonObject KeyringManager::handleResponse(const QString& cookie, const QString& response) {
        auto it = m_pendingRequests.find(cookie);
        if (it == m_pendingRequests.end()) {
            return QJsonObject{{"type", "error"}, {"message", "Unknown cookie"}};
        }

        m_pendingRequests.erase(it);

        // Close session via Agent
        g_pAgent->closeSession(cookie, bb::Session::Result::Success);

        return QJsonObject{{"type", "keyring_response"}, {"id", cookie}, {"result", "ok"}, {"password", response}};
    }

    QJsonObject KeyringManager::handleCancel(const QString& cookie) {
        auto it = m_pendingRequests.find(cookie);
        if (it == m_pendingRequests.end()) {
            return QJsonObject{{"type", "error"}, {"message", "Unknown cookie"}};
        }

        m_pendingRequests.erase(it);

        // Close session via Agent
        g_pAgent->closeSession(cookie, bb::Session::Result::Cancelled);

        return QJsonObject{{"type", "keyring_response"}, {"result", "cancelled"}, {"id", cookie}};
    }

    bool KeyringManager::hasPendingRequest(const QString& cookie) const {
        return m_pendingRequests.contains(cookie);
    }

    QLocalSocket* KeyringManager::getSocketForRequest(const QString& cookie) const {
        auto it = m_pendingRequests.find(cookie);
        return (it != m_pendingRequests.end()) ? it->socket : nullptr;
    }

    void KeyringManager::cleanupForSocket(QLocalSocket* socket) {
        for (auto it = m_pendingRequests.begin(); it != m_pendingRequests.end();) {
            if (it->socket == socket) {
                QString cookie = it->cookie;
                it = m_pendingRequests.erase(it);

                // Close session via Agent
                g_pAgent->closeSession(cookie, bb::Session::Result::Cancelled);
            } else {
                ++it;
            }
        }
    }

} // namespace bb
