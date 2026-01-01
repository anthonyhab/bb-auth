#include "core/Agent.hpp"

#include <QCommandLineParser>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTextStream>

namespace {
QString defaultSocketPath() {
    const auto runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    return runtimeDir + "/noctalia-polkit-agent.sock";
}

bool sendJsonCommand(const QString& socketPath, const QJsonObject& request, QJsonObject* response) {
    QLocalSocket socket;
    socket.connectToServer(socketPath);
    if (!socket.waitForConnected(1000))
        return false;

    QByteArray data = QJsonDocument(request).toJson(QJsonDocument::Compact);
    data.append("\n");

    if (socket.write(data) == -1 || !socket.waitForBytesWritten(1000))
        return false;

    if (!socket.waitForReadyRead(1000))
        return false;
    if (!socket.canReadLine() && !socket.waitForReadyRead(1000))
        return false;

    const QByteArray replyLine = socket.readLine().trimmed();
    if (replyLine.isEmpty())
        return false;

    QJsonParseError parseError;
    const auto      doc = QJsonDocument::fromJson(replyLine, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return false;

    if (response)
        *response = doc.object();
    return true;
}
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription("Noctalia Polkit Agent");
    parser.addHelpOption();

    QCommandLineOption optPing(QStringList{"ping"}, "Check if the daemon is reachable.");
    QCommandLineOption optNext(QStringList{"next"}, "Fetch the next pending request.");
    QCommandLineOption optRespond(QStringList{"respond"}, "Respond to a request (cookie).", "cookie");
    QCommandLineOption optCancel(QStringList{"cancel"}, "Cancel a request (cookie).", "cookie");
    QCommandLineOption optSocket(QStringList{"socket"}, "Override socket path.", "path");

    parser.addOption(optPing);
    parser.addOption(optNext);
    parser.addOption(optRespond);
    parser.addOption(optCancel);
    parser.addOption(optSocket);

    parser.process(app);

    const QString socketPath = parser.isSet(optSocket) ? parser.value(optSocket) : defaultSocketPath();

    if (parser.isSet(optPing)) {
        QJsonObject response;
        const bool  ok = sendJsonCommand(socketPath, QJsonObject{{"type", "ping"}}, &response);
        return (ok && response.value("type").toString() == "pong") ? 0 : 1;
    }

    if (parser.isSet(optNext)) {
        QJsonObject response;
        const bool  ok = sendJsonCommand(socketPath, QJsonObject{{"type", "next"}}, &response);
        if (ok && response.value("type").toString() != "empty") {
            const auto out = QJsonDocument(response).toJson(QJsonDocument::Compact);
            fprintf(stdout, "%s\n", out.constData());
        }
        return ok ? 0 : 1;
    }

    if (parser.isSet(optRespond)) {
        const QString cookie = parser.value(optRespond);
        QTextStream stdinStream(stdin);
        const QString password = stdinStream.readLine();
        QJsonObject response;
        const bool  ok = sendJsonCommand(socketPath,
                                         QJsonObject{{"type", "respond"}, {"id", cookie}, {"response", password}},
                                         &response);
        return (ok && response.value("type").toString() == "ok") ? 0 : 1;
    }

    if (parser.isSet(optCancel)) {
        const QString cookie = parser.value(optCancel);
        QJsonObject response;
        const bool  ok = sendJsonCommand(socketPath,
                                         QJsonObject{{"type", "cancel"}, {"id", cookie}},
                                         &response);
        return (ok && response.value("type").toString() == "ok") ? 0 : 1;
    }

    g_pAgent = std::make_unique<CAgent>();
    return g_pAgent->start(app, socketPath) == false ? 1 : 0;
}
