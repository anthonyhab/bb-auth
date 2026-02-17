#include "PinentryManager.hpp"
#include "../Agent.hpp"
#include "../../common/Constants.hpp"

#include <QDebug>
#include <QList>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>

#include <optional>
#include <unistd.h>

namespace bb {

namespace {

PinentryRequest parsePinentryRequest(const QJsonObject& msg, QLocalSocket* socket, pid_t peerPid) {
    PinentryRequest request;
    request.cookie = msg.value("cookie").toString();
    request.socket = socket;
    request.peerPid = peerPid;

    request.prompt = msg.value("prompt").toString();
    if (request.prompt.isEmpty()) {
        request.prompt = "Enter passphrase:";
    }

    request.description = msg.value("description").toString();

    request.error = msg.value("error").toString();

    request.keyinfo = msg.value("keyinfo").toString();

    request.repeat = msg.value("repeat").toBool();

    request.confirmOnly = msg.value("confirm_only").toBool();

    return request;
}

ActorInfo resolveActorForPid(pid_t peerPid) {
    std::optional<ProcInfo> proc = RequestContextHelper::readProc(peerPid);
    if (!proc) {
        return {};
    }

    return RequestContextHelper::resolveRequestorFromSubject(*proc, getuid());
}

} // namespace

PinentryManager::PinentryManager(QObject* parent) : QObject(parent) {}

PinentryManager::~PinentryManager() = default;

void PinentryManager::handleRequest(const QJsonObject& msg, QLocalSocket* socket, pid_t peerPid) {
    PinentryRequest request = parsePinentryRequest(msg, socket, peerPid);
    if (request.cookie.isEmpty()) {
        request.cookie = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    const QString cookie = request.cookie;

    if (m_flowOwners.contains(cookie) && m_flowOwners.value(cookie) != peerPid) {
        qWarning() << "Pinentry owner mismatch for cookie" << cookie << "expected pid"
                   << m_flowOwners.value(cookie) << "got" << peerPid;
        return;
    }

    m_flowOwners[cookie] = peerPid;
    if (!request.keyinfo.isEmpty()) {
        m_flowKeyinfos[cookie] = request.keyinfo;
    }

    const auto [curRetry, maxRetries] = resolveRetryInfo(request);
    const bool sessionExists = g_pAgent->getSession(cookie) != nullptr;

    if (m_awaitingOutcome.contains(cookie)) {
        cleanupAwaiting(cookie);
        const QString retryError = request.error.isEmpty() ? QString("Authentication failed") : request.error;
        g_pAgent->updateSessionError(cookie, retryError);
    }

    m_pendingRequests[cookie] = request;

    if (!sessionExists) {
        const ActorInfo actor = resolveActorForPid(peerPid);

        Session::Context ctx;
        ctx.message = request.prompt;
        ctx.description = request.description;
        ctx.keyinfo = request.keyinfo;
        ctx.curRetry = curRetry;
        ctx.maxRetries = maxRetries;
        ctx.confirmOnly = request.confirmOnly;
        ctx.repeat = request.repeat;
        ctx.requestor.name = actor.displayName;
        ctx.requestor.icon = actor.iconName;
        ctx.requestor.fallbackLetter = actor.fallbackLetter;
        ctx.requestor.fallbackKey = actor.fallbackKey;
        ctx.requestor.pid = peerPid;

        if (!g_pAgent->createSession(cookie, Session::Source::Pinentry, ctx)) {
            // Should not happen as we checked !sessionExists earlier, but for safety:
            qWarning() << "Failed to create pinentry session (collision?):" << cookie;
            m_pendingRequests.remove(cookie);
            m_flowOwners.remove(cookie);
            if (!request.keyinfo.isEmpty()) {
                m_flowKeyinfos.remove(cookie);
            }
            QJsonObject error{{"type", "error"}, {"message", "Session ID collision"}};
            QJsonDocument doc(error);
            if (socket && socket->isOpen()) {
                socket->write(doc.toJson(QJsonDocument::Compact) + "\n");
                socket->flush();
            }
            return;
        }
    } else {
        g_pAgent->updateSessionPinentryRetry(cookie, curRetry, maxRetries);
    }

    g_pAgent->updateSessionPrompt(cookie, request.prompt, false, false);

    bool shouldEmitRequestError = !request.error.isEmpty();
    if (m_retryReported.contains(cookie)) {
        if (shouldEmitRequestError) {
            shouldEmitRequestError = false;
        }
        m_retryReported.remove(cookie);
    }

    if (shouldEmitRequestError) {
        g_pAgent->updateSessionError(cookie, request.error);
    }
}

PinentryManager::ResponseResult PinentryManager::handleResponse(const QString& cookie, const QString& response) {
    auto pendingIt = m_pendingRequests.find(cookie);
    if (pendingIt == m_pendingRequests.end()) {
        if (m_awaitingOutcome.contains(cookie)) {
            return {QJsonObject{{"type", "error"}, {"message", "Session is already awaiting terminal result"}}};
        }
        return {QJsonObject{{"type", "error"}, {"message", "Unknown session"}}};
    }

    PinentryRequest request = pendingIt.value();
    m_pendingRequests.erase(pendingIt);

    QJsonObject socketResponse;
    socketResponse["type"] = "pinentry_response";
    socketResponse["id"] = cookie;
    if (request.confirmOnly) {
        socketResponse["result"] = "confirmed";
    } else {
        socketResponse["result"] = "ok";
        socketResponse["password"] = response;
    }

    cleanupAwaiting(cookie);

    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, cookie]() {
        closeFlow(cookie, Session::Result::Error, "Pinentry did not report terminal result");
    });

    AwaitingOutcome awaiting;
    awaiting.request = request;
    awaiting.timer = timer;
    m_awaitingOutcome[cookie] = awaiting;
    timer->start(PINENTRY_RESULT_TIMEOUT_MS);

    return {socketResponse};
}

QJsonObject PinentryManager::handleResult(const QJsonObject& msg, pid_t peerPid) {
    const QString cookie = msg.value("id").toString();
    if (cookie.isEmpty()) {
        return QJsonObject{{"type", "error"}, {"message", "Missing id"}};
    }

    if (!validateResultOwner(cookie, peerPid)) {
        return QJsonObject{{"type", "error"}, {"message", "Result sender does not own session"}};
    }

    Session* session = g_pAgent->getSession(cookie);
    if (!session || session->source() != Session::Source::Pinentry) {
        return QJsonObject{{"type", "error"}, {"message", "Unknown pinentry session"}};
    }

    const QString result = msg.value("result").toString().toLower();
    const QString error = msg.value("error").toString();

    if (result == "success") {
        closeFlow(cookie, Session::Result::Success);
        return QJsonObject{{"type", "ok"}};
    }

    if (result == "retry") {
        cleanupAwaiting(cookie);
        const QString reason = error.isEmpty() ? QString("Authentication failed") : error;
        m_retryReported.insert(cookie);
        g_pAgent->updateSessionError(cookie, reason);
        return QJsonObject{{"type", "ok"}};
    }

    if (result == "cancelled" || result == "canceled") {
        closeFlow(cookie, Session::Result::Cancelled);
        return QJsonObject{{"type", "ok"}};
    }

    if (result == "error") {
        const QString reason = error.isEmpty() ? QString("Authentication failed") : error;
        closeFlow(cookie, Session::Result::Error, reason);
        return QJsonObject{{"type", "ok"}};
    }

    return QJsonObject{{"type", "error"}, {"message", "Invalid result type"}};
}

QJsonObject PinentryManager::handleCancel(const QString& cookie) {
    Session* session = g_pAgent->getSession(cookie);

    if (m_pendingRequests.contains(cookie)) {
        closeFlow(cookie, Session::Result::Cancelled);
        return QJsonObject{{"type", "pinentry_response"}, {"id", cookie}, {"result", "cancelled"}};
    }

    if (m_awaitingOutcome.contains(cookie)) {
        closeFlow(cookie, Session::Result::Cancelled);
        return QJsonObject{{"type", "pinentry_response"}, {"id", cookie}, {"result", "cancelled"}};
    }

    if (session && session->source() == Session::Source::Pinentry) {
        closeFlow(cookie, Session::Result::Cancelled);
        return QJsonObject{{"type", "pinentry_response"}, {"id", cookie}, {"result", "cancelled"}};
    }

    return QJsonObject{{"type", "error"}, {"message", "Unknown session"}};
}

bool PinentryManager::hasPendingInput(const QString& cookie) const {
    return m_pendingRequests.contains(cookie);
}

bool PinentryManager::hasRequest(const QString& cookie) const {
    if (m_pendingRequests.contains(cookie) || m_awaitingOutcome.contains(cookie)) {
        return true;
    }

    Session* session = g_pAgent->getSession(cookie);
    return session && session->source() == Session::Source::Pinentry;
}

bool PinentryManager::isAwaitingOutcome(const QString& cookie) const {
    return m_awaitingOutcome.contains(cookie);
}

QLocalSocket* PinentryManager::getSocketForPendingInput(const QString& cookie) const {
    auto it = m_pendingRequests.find(cookie);
    if (it == m_pendingRequests.end()) {
        return nullptr;
    }

    return it->socket;
}

void PinentryManager::cleanupForSocket(QLocalSocket* socket) {
    QList<QString> cookiesToClose;
    for (auto it = m_pendingRequests.cbegin(); it != m_pendingRequests.cend(); ++it) {
        if (it->socket == socket) {
            cookiesToClose.push_back(it.key());
        }
    }

    for (const QString& cookie : cookiesToClose) {
        closeFlow(cookie, Session::Result::Cancelled, "Pinentry disconnected");
    }
}

std::pair<int, int> PinentryManager::resolveRetryInfo(const PinentryRequest& request) {
    int curRetry = 0;
    int maxRetries = 3;
    bool parsed = false;

    static const QRegularExpression retryRe(R"(\((\d+)\s+of\s+(\d+)\s+attempts\))");
    const auto match = retryRe.match(request.description);
    if (match.hasMatch()) {
        curRetry = match.captured(1).toInt();
        maxRetries = match.captured(2).toInt();
        parsed = true;
    }

    if (!request.keyinfo.isEmpty()) {
        auto& info = m_retryInfo[request.keyinfo];
        info.keyinfo = request.keyinfo;

        if (parsed) {
            info.curRetry = curRetry;
            info.maxRetries = maxRetries;
        } else {
            curRetry = info.curRetry;
            maxRetries = info.maxRetries > 0 ? info.maxRetries : 3;
        }
    }

    if (curRetry < 0) {
        curRetry = 0;
    }
    if (maxRetries <= 0) {
        maxRetries = 3;
    }

    return {curRetry, maxRetries};
}

bool PinentryManager::validateResultOwner(const QString& cookie, pid_t peerPid) const {
    auto ownerIt = m_flowOwners.find(cookie);
    if (ownerIt == m_flowOwners.end()) {
        return true;
    }

    return ownerIt.value() == peerPid;
}

void PinentryManager::cleanupAwaiting(const QString& cookie) {
    auto it = m_awaitingOutcome.find(cookie);
    if (it == m_awaitingOutcome.end()) {
        return;
    }

    if (it->timer) {
        it->timer->stop();
        it->timer->deleteLater();
    }

    m_awaitingOutcome.erase(it);
}

void PinentryManager::closeFlow(const QString& cookie, Session::Result result, const QString& error) {
    Session* session = g_pAgent->getSession(cookie);
    if (session && session->source() == Session::Source::Pinentry) {
        if (!error.isEmpty()) {
            g_pAgent->updateSessionError(cookie, error);
        }
        g_pAgent->closeSession(cookie, result);
    }

    m_pendingRequests.remove(cookie);
    cleanupAwaiting(cookie);
    m_flowOwners.remove(cookie);
    m_retryReported.remove(cookie);

    const QString keyinfo = m_flowKeyinfos.take(cookie);
    if (!keyinfo.isEmpty()) {
        m_retryInfo.remove(keyinfo);
    }
}

} // namespace bb
