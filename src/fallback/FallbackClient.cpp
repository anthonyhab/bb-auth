#include "FallbackClient.hpp"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonParseError>

namespace noctalia {

FallbackClient::FallbackClient(const QString& socketPath, QObject* parent)
    : QObject(parent)
    , m_socketPath(socketPath) {

    connect(&m_socket, &QLocalSocket::connected, this, [this]() {
        m_reconnectDelayMs = 200;
        m_subscribed = false;
        m_registered = false;
        m_providerId.clear();
        m_pendingProviderActiveKnown = false;
        m_pendingProviderActive = false;
        m_pendingProviderId.clear();
        setProviderActive(false);

        emit connectionStateChanged(true);
        emit statusMessage("Connected to auth daemon");

        registerProvider();
        subscribe();
    });

    connect(&m_socket, &QLocalSocket::disconnected, this, [this]() {
        m_subscribed = false;
        m_registered = false;
        m_providerId.clear();
        m_pendingProviderActiveKnown = false;
        m_pendingProviderActive = false;
        m_pendingProviderId.clear();
        setProviderActive(false);

        emit connectionStateChanged(false);
        emit statusMessage("Disconnected from auth daemon, reconnecting...");

        if (!m_reconnectTimer.isActive()) {
            m_reconnectTimer.start(m_reconnectDelayMs);
            m_reconnectDelayMs = qMin(m_reconnectDelayMs * 2, 4000);
        }
    });

    connect(&m_socket, &QLocalSocket::readyRead, this, [this]() {
        m_buffer.append(m_socket.readAll());

        int newline = -1;
        while ((newline = m_buffer.indexOf('\n')) != -1) {
            QByteArray line = m_buffer.left(newline).trimmed();
            m_buffer.remove(0, newline + 1);

            if (line.isEmpty()) {
                continue;
            }

            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
            if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
                emit statusMessage("Invalid daemon payload");
                continue;
            }

            handleMessage(doc.object());
        }
    });

    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
        if (m_socket.state() != QLocalSocket::ConnectedState && !m_reconnectTimer.isActive()) {
            m_reconnectTimer.start(m_reconnectDelayMs);
            m_reconnectDelayMs = qMin(m_reconnectDelayMs * 2, 4000);
        }
    });

    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this]() { ensureConnected(); });

    m_subscribeWatchdog.setInterval(1200);
    m_subscribeWatchdog.setSingleShot(false);
    connect(&m_subscribeWatchdog, &QTimer::timeout, this, [this]() {
        if (!isConnected()) {
            return;
        }

        if (!m_registered) {
            registerProvider();
        }

        if (!m_subscribed) {
            subscribe();
        }
    });

    m_heartbeatTimer.setInterval(4000);
    m_heartbeatTimer.setSingleShot(false);
    connect(&m_heartbeatTimer, &QTimer::timeout, this, [this]() {
        if (!isConnected() || !m_registered) {
            return;
        }

        QJsonObject heartbeat{
            {"type", "ui.heartbeat"},
            {"id", m_providerId}
        };
        sendJson(heartbeat);
    });
}

void FallbackClient::start() {
    m_subscribeWatchdog.start();
    m_heartbeatTimer.start();
    ensureConnected();
}

bool FallbackClient::isConnected() const {
    return m_socket.state() == QLocalSocket::ConnectedState;
}

bool FallbackClient::isActiveProvider() const {
    return m_providerActive;
}

void FallbackClient::sendResponse(const QString& id, const QString& response) {
    QJsonObject req{
        {"type", "session.respond"},
        {"id", id},
        {"response", response}
    };
    sendJson(req);
}

void FallbackClient::sendCancel(const QString& id) {
    QJsonObject req{
        {"type", "session.cancel"},
        {"id", id}
    };
    sendJson(req);
}

void FallbackClient::ensureConnected() {
    if (isConnected() || m_socket.state() == QLocalSocket::ConnectingState) {
        return;
    }

    m_socket.connectToServer(m_socketPath);
}

void FallbackClient::sendJson(const QJsonObject& json) {
    if (!isConnected()) {
        return;
    }

    QByteArray payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    payload.append('\n');
    m_socket.write(payload);
    m_socket.flush();
}

void FallbackClient::registerProvider() {
    QJsonObject reg{
        {"type", "ui.register"},
        {"name", "noctalia-auth-fallback"},
        {"kind", "fallback"},
        {"priority", 10}
    };
    sendJson(reg);
}

void FallbackClient::subscribe() {
    sendJson(QJsonObject{{"type", "subscribe"}});
}

void FallbackClient::setProviderActive(bool active) {
    const bool changed = (active != m_providerActive);
    if (!changed) {
        return;
    }

    m_providerActive = active;
    emit providerStateChanged(m_providerActive);

    if (m_providerActive && isConnected() && m_registered) {
        subscribe();
    }
}

void FallbackClient::applyPendingProviderState() {
    if (!m_pendingProviderActiveKnown || !m_registered || m_providerId.isEmpty()) {
        return;
    }

    bool shouldBeActive = true;

    if (m_pendingProviderActive) {
        shouldBeActive = (m_pendingProviderId == m_providerId);
    }

    setProviderActive(shouldBeActive);

    m_pendingProviderActiveKnown = false;
    m_pendingProviderActive = false;
    m_pendingProviderId.clear();
}

void FallbackClient::handleMessage(const QJsonObject& msg) {
    const QString type = msg.value("type").toString();

    if (type == "subscribed") {
        m_subscribed = true;
        if (msg.contains("active")) {
            setProviderActive(msg.value("active").toBool());
        }
        return;
    }

    if (type == "ui.registered") {
        m_registered = true;
        m_providerId = msg.value("id").toString();
        if (msg.contains("active")) {
            const bool active = msg.value("active").toBool();
            setProviderActive(active);

            m_pendingProviderActiveKnown = false;
            m_pendingProviderActive = false;
            m_pendingProviderId.clear();
        } else {
            applyPendingProviderState();
        }
        return;
    }

    if (type == "ui.active") {
        const bool active = msg.value("active").toBool();
        const QString activeId = msg.value("id").toString();

        if (active && (!m_registered || m_providerId.isEmpty())) {
            m_pendingProviderActiveKnown = true;
            m_pendingProviderActive = true;
            m_pendingProviderId = activeId;
            return;
        }

        if (!active) {
            if (!m_registered) {
                m_pendingProviderActiveKnown = true;
                m_pendingProviderActive = false;
                m_pendingProviderId.clear();
                return;
            }

            setProviderActive(true);
            m_pendingProviderActiveKnown = false;
            m_pendingProviderActive = false;
            m_pendingProviderId.clear();
            return;
        }

        const bool newActive = (activeId == m_providerId);
        setProviderActive(newActive);
        return;
    }

    if (type == "pong") {
        return;
    }

    if (type == "ok") {
        return;
    }

    if (type == "error") {
        if (msg.value("message").toString() == "Not active UI provider") {
            setProviderActive(false);
        }
        emit statusMessage(msg.value("message").toString());
        return;
    }

    if (!m_providerActive && m_registered) {
        return;
    }

    if (type == "session.created") {
        emit sessionCreated(msg);
    } else if (type == "session.updated") {
        emit sessionUpdated(msg);
    } else if (type == "session.closed") {
        emit sessionClosed(msg);
    }
}

} // namespace noctalia
