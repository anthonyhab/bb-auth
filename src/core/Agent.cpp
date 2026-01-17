#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE 1

#include <print>
#include <cstddef>
#include <QDBusConnection>

namespace {
    void secureZero(void* ptr, std::size_t len) {
        volatile unsigned char* p = static_cast<volatile unsigned char*>(ptr);
        while (len--)
            *p++ = 0;
    }
} // namespace

#include <QDBusInterface>
#include <QDBusReply>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QStringList>
#ifdef signals
#undef signals
#endif
#include <polkitagent/polkitagent.h>
#include <sys/socket.h>
#include <unistd.h>

#include "Agent.hpp"
#include "RequestContext.hpp"

bool CAgent::start(QCoreApplication& app, const QString& socketPath) {
    sessionSubject = std::make_shared<PolkitQt1::UnixSessionSubject>(getpid());

    listener.registerListener(*sessionSubject, "/org/noctalia/PolicyKit1/AuthenticationAgent");

    app.setApplicationName("Noctalia Polkit Agent");
    ipcSocketPath = socketPath;
    setupIpcServer();

    fingerprintAvailable = checkFingerprintAvailable();
    if (fingerprintAvailable)
        std::print("Fingerprint authentication available\n");

    app.exec();

    return true;
}

bool CAgent::checkFingerprintAvailable() {
    // Check if fprintd is available and user has enrolled fingerprints
    QDBusInterface manager("net.reactivated.Fprint", "/net/reactivated/Fprint/Manager", "net.reactivated.Fprint.Manager", QDBusConnection::systemBus());

    if (!manager.isValid())
        return false;

    // Get the default fingerprint device
    QDBusReply<QDBusObjectPath> deviceReply = manager.call("GetDefaultDevice");
    if (!deviceReply.isValid())
        return false;

    QString devicePath = deviceReply.value().path();
    if (devicePath.isEmpty())
        return false;

    // Check if current user has enrolled fingerprints on this device
    QDBusInterface device("net.reactivated.Fprint", devicePath, "net.reactivated.Fprint.Device", QDBusConnection::systemBus());

    if (!device.isValid())
        return false;

    // ListEnrolledFingers returns the list of enrolled fingers for a user
    QString                 username     = qgetenv("USER");
    QDBusReply<QStringList> fingersReply = device.call("ListEnrolledFingers", username);

    if (!fingersReply.isValid())
        return false;

    return !fingersReply.value().isEmpty();
}

void CAgent::initAuthPrompt(const QString& cookie) {
    if (!listener.sessions.contains(cookie)) {
        std::print(stderr, "INTERNAL ERROR: Auth prompt requested but session {} isn't in progress\n", cookie.toStdString());
        return;
    }

    std::print("Auth prompt requested for {}\n", cookie.toStdString());
    // The actual request is emitted when the session provides a prompt.
}

void CAgent::enqueueEvent(const QJsonObject& event) {
    std::print("Enqueuing event: {}\n", QJsonDocument(event).toJson(QJsonDocument::Indented).toStdString());
    eventQueue.enqueue(event);
}

QJsonObject CAgent::buildRequestEvent(const QString& cookie) const {
    if (!listener.sessions.contains(cookie))
        return {};

    const auto& session = *listener.sessions[cookie];

    QJsonObject event;
    event["type"]                 = "request";
    event["source"]               = "polkit";
    event["id"]                   = session.cookie;
    event["actionId"]             = session.actionId;
    event["message"]              = session.message;
    event["icon"]                 = session.iconName;
    event["user"]                 = session.selectedUser.toString();
    event["prompt"]               = RequestContextHelper::normalizePrompt(session.prompt);
    event["echo"]                 = session.echoOn;
    event["fingerprintAvailable"] = fingerprintAvailable;

    QJsonObject details;
    const auto  keys = session.details.keys();
    for (const auto& key : keys) {
        details.insert(key, session.details.lookup(key));
    }
    event["details"] = details;

    // Rich context
    ActorInfo requestor;
    auto      pid = RequestContextHelper::extractSubjectPid(session.details);
    if (pid) {
        auto subject = RequestContextHelper::readProc(*pid);
        if (subject) {
            event["subject"]   = subject->toJson();
            requestor          = RequestContextHelper::resolveRequestorFromSubject(*subject, getuid());
            event["requestor"] = requestor.toJson();

            std::print("Resolved requestor: {} (confidence: {})\n", requestor.displayName.toStdString(), requestor.confidence.toStdString());
        }
    } else {
        std::print("No PID found in polkit details for requestor resolution\n");
    }

    if (!session.errorText.isEmpty())
        event["error"] = session.errorText;

    // Hint
    event["hint"] = RequestContextHelper::classifyRequest("polkit", event["actionId"].toString(), event["message"].toString(), requestor);

    return event;
}

QJsonObject CAgent::buildKeyringRequestEvent(const KeyringRequest& req) const {
    QJsonObject event;
    event["type"]                 = "request";
    event["source"]               = "keyring";
    event["id"]                   = req.cookie;
    event["message"]              = req.title;
    event["prompt"]               = req.message;
    event["echo"]                 = false;
    event["passwordNew"]          = req.passwordNew;
    event["confirmOnly"]          = req.confirmOnly;
    event["fingerprintAvailable"] = fingerprintAvailable;

    if (!req.description.isEmpty())
        event["description"] = req.description;

    return event;
}

void CAgent::enqueueRequest(const QString& cookie) {
    enqueueEvent(buildRequestEvent(cookie));
}

void CAgent::enqueueError(const QString& cookie, const QString& error) {
    QJsonObject event;
    event["type"]  = "update";
    event["id"]    = cookie;
    event["error"] = error;
    enqueueEvent(event);
}

void CAgent::enqueueComplete(const QString& cookie, const QString& result) {
    QJsonObject event;
    event["type"]   = "complete";
    event["id"]     = cookie;
    event["result"] = result;
    enqueueEvent(event);
}

bool CAgent::handleRespond(const QString& cookie, const QString& password) {
    if (!listener.sessions.contains(cookie))
        return false;
    listener.submitPassword(cookie, password);
    return true;
}

bool CAgent::handleCancel(const QString& cookie) {
    if (!listener.sessions.contains(cookie))
        return false;
    listener.cancelPending(cookie);
    return true;
}

void CAgent::setupIpcServer() {
    if (ipcSocketPath.isEmpty()) {
        const auto runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        ipcSocketPath         = runtimeDir + "/noctalia-polkit-agent.sock";
    }

    QLocalServer::removeServer(ipcSocketPath);

    ipcServer = new QLocalServer();
    ipcServer->setSocketOptions(QLocalServer::UserAccessOption);

    QObject::connect(ipcServer, &QLocalServer::newConnection, [this]() {
        while (ipcServer->hasPendingConnections()) {
            auto* socket = ipcServer->nextPendingConnection();
            QObject::connect(socket, &QLocalSocket::readyRead, [this, socket]() {
                while (socket->canReadLine()) {
                    QByteArray line = socket->readLine().trimmed();
                    if (line.isEmpty())
                        continue;
                    handleSocketLine(socket, line);
                }
            });
            QObject::connect(socket, &QLocalSocket::disconnected, [this, socket]() {
                cleanupKeyringRequestsForSocket(socket);
                socket->deleteLater();
            });
        }
    });

    if (!ipcServer->listen(ipcSocketPath)) {
        std::print(stderr, "IPC listen failed on {}: {}\n", ipcSocketPath.toStdString(), ipcServer->errorString().toStdString());
        return;
    }

    std::print("IPC listening on {}\n", ipcSocketPath.toStdString());
}

void CAgent::handleSocketLine(QLocalSocket* socket, const QByteArray& line) {
    // Input validation: reject oversized messages
    constexpr qsizetype MAX_MESSAGE_SIZE = 64 * 1024; // 64KB
    if (line.size() > MAX_MESSAGE_SIZE) {
        std::print(stderr, "Rejected oversized message: {} bytes\n", line.size());
        sendJson(socket, QJsonObject{{"type", "error"}, {"error", "message_too_large"}});
        socket->disconnectFromServer();
        return;
    }

    QJsonParseError parseError;
    QJsonDocument   doc = QJsonDocument::fromJson(line, &parseError);

    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        std::print(stderr, "IPC JSON parse error: {}\n", parseError.errorString().toStdString());
        sendJson(socket, QJsonObject{{"type", "error"}, {"error", "invalid_json"}});
        return;
    }

    const QJsonObject obj  = doc.object();
    const QString     type = obj.value("type").toString();

    if (type == "ping") {
        sendJson(socket, QJsonObject{{"type", "pong"}});
        return;
    }

    if (type == "next") {
        if (eventQueue.isEmpty()) {
            sendJson(socket, QJsonObject{{"type", "empty"}});
        } else {
            const auto event = eventQueue.dequeue();
            sendJson(socket, event);
        }
        return;
    }

    if (type == "keyring_request") {
        handleKeyringRequest(socket, obj);
        // Keep socket open for response.
        return;
    }

    if (type == "respond") {
        const QString cookie   = obj.value("id").toString();
        const QString response = obj.value("response").toString();

        if (cookie.isEmpty()) {
            sendJson(socket, QJsonObject{{"type", "error"}, {"error", "missing_id"}});
            return;
        }

        if (pendingKeyringRequests.contains(cookie)) {
            respondToKeyringRequest(cookie, response);
            sendJson(socket, QJsonObject{{"type", "ok"}});
            return;
        }

        const bool ok = handleRespond(cookie, response);
        sendJson(socket, ok ? QJsonObject{{"type", "ok"}} : QJsonObject{{"type", "error"}, {"error", "invalid_cookie"}});
        return;
    }

    if (type == "cancel") {
        const QString cookie = obj.value("id").toString();

        if (cookie.isEmpty()) {
            sendJson(socket, QJsonObject{{"type", "error"}, {"error", "missing_id"}});
            return;
        }

        if (pendingKeyringRequests.contains(cookie)) {
            cancelKeyringRequest(cookie);
            sendJson(socket, QJsonObject{{"type", "ok"}});
            return;
        }

        const bool ok = handleCancel(cookie);
        sendJson(socket, ok ? QJsonObject{{"type", "ok"}} : QJsonObject{{"type", "error"}, {"error", "invalid_cookie"}});
        return;
    }

    sendJson(socket, QJsonObject{{"type", "error"}, {"error", "unknown_command"}});
}

void CAgent::sendJson(QLocalSocket* socket, const QJsonObject& obj, bool disconnect, bool secure) {
    if (!socket || socket->state() != QLocalSocket::ConnectedState)
        return;

    QByteArray json = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    socket->write(json);
    socket->write("\n");
    socket->flush();

    if (secure)
        secureZero(json.data(), json.size());

    if (disconnect)
        socket->disconnectFromServer();
}

void CAgent::handleKeyringRequest(QLocalSocket* socket, const QJsonObject& obj) {
    KeyringRequest req;
    req.cookie      = obj.value("cookie").toString();
    req.title       = obj.value("title").toString();
    req.message     = obj.value("message").toString();
    req.description = obj.value("description").toString();
    req.passwordNew = obj.value("password_new").toBool(false);
    req.confirmOnly = obj.value("confirm_only").toBool(false);
    req.replySocket = socket;

    if (req.cookie.isEmpty()) {
        std::print(stderr, "Keyring request missing cookie\n");
        sendJson(socket, QJsonObject{{"type", "error"}, {"error", "missing_cookie"}});
        return;
    }

    std::print("Keyring request received: cookie={} title={}\n", req.cookie.toStdString(), req.title.toStdString());

    // Try to parse origin PID from title: [PID]@host
    if (req.title.startsWith('[')) {
        int end = req.title.indexOf(']');
        if (end > 1) {
            bool   ok  = false;
            qint64 pid = req.title.mid(1, end - 1).toLongLong(&ok);
            if (ok) {
                req.originPid = pid;
                std::print("Parsed origin PID from title: {}\n", pid);
            }
        }
    }

    pendingKeyringRequests[req.cookie] = req;

    // Enqueue event for UI
    QJsonObject event = buildKeyringRequestEvent(req);

    // Try to get peer PID for keyring requestor
    qint64 targetPid = 0;
    if (req.originPid > 0) {
        targetPid = req.originPid;
    } else {
        struct ucred ucred;
        socklen_t    len = sizeof(struct ucred);
        if (getsockopt(socket->socketDescriptor(), SOL_SOCKET, SO_PEERCRED, &ucred, &len) != -1) {
            targetPid = ucred.pid;
        }
    }

    if (targetPid > 0) {
        auto subject = RequestContextHelper::readProc(targetPid);
        if (subject) {
            event["subject"]   = subject->toJson();
            auto requestor     = RequestContextHelper::resolveRequestorFromSubject(*subject, getuid());
            event["requestor"] = requestor.toJson();
            event["hint"]      = RequestContextHelper::classifyRequest("keyring", req.title, req.description, requestor);
        }
    }

    if (!event.contains("hint")) {
        event["hint"] = RequestContextHelper::classifyRequest("keyring", req.title, req.description, {});
    }

    enqueueEvent(event);
}

void CAgent::cleanupKeyringRequestsForSocket(QLocalSocket* socket) {
    if (!socket)
        return;

    QStringList staleCookies;
    staleCookies.reserve(pendingKeyringRequests.size());
    for (auto it = pendingKeyringRequests.cbegin(); it != pendingKeyringRequests.cend(); ++it) {
        if (it->replySocket == socket)
            staleCookies.append(it.key());
    }

    for (const auto& cookie : staleCookies)
        cancelKeyringRequest(cookie);
}

void CAgent::respondToKeyringRequest(const QString& cookie, const QString& password) {
    if (!pendingKeyringRequests.contains(cookie)) {
        std::print(stderr, "Keyring respond: unknown cookie {}\n", cookie.toStdString());
        return;
    }

    KeyringRequest req = pendingKeyringRequests.take(cookie);

    std::print("Responding to keyring request: cookie={}\n", cookie.toStdString());

    if (req.replySocket && req.replySocket->isOpen()) {
        QJsonObject response;
        response["type"]   = "keyring_response";
        response["id"]     = cookie;
        response["result"] = req.confirmOnly ? "confirmed" : "ok";
        if (!req.confirmOnly)
            response["password"] = password;
        sendJson(req.replySocket, response, true, !req.confirmOnly);
    }

    // Notify UI that the request is complete
    QJsonObject event;
    event["type"]   = "complete";
    event["id"]     = cookie;
    event["result"] = "success";
    enqueueEvent(event);
}

void CAgent::cancelKeyringRequest(const QString& cookie) {
    if (!pendingKeyringRequests.contains(cookie)) {
        std::print(stderr, "Keyring cancel: unknown cookie {}\n", cookie.toStdString());
        return;
    }

    KeyringRequest req = pendingKeyringRequests.take(cookie);

    std::print("Cancelling keyring request: cookie={}\n", cookie.toStdString());

    if (req.replySocket && req.replySocket->isOpen()) {
        QJsonObject response;
        response["type"]   = "keyring_response";
        response["id"]     = cookie;
        response["result"] = "cancelled";
        sendJson(req.replySocket, response);
    }

    // Notify UI that the request is complete (cancelled)
    QJsonObject event;
    event["type"]   = "complete";
    event["id"]     = cookie;
    event["result"] = "cancelled";
    enqueueEvent(event);
}
