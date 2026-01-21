#include "common/Paths.hpp"
#include "core/Agent.hpp"
#include "modes/daemon.hpp"
#include "modes/pinentry.hpp"

#include <QCommandLineParser>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTextStream>

#include <print>

// Forward declarations for mode runners
namespace modes {
    int runKeyring(int argc, char* argv[]);
    int runPinentry();
}

namespace {

    enum class Mode {
        Daemon,
        Keyring,
        Pinentry,
        Cli
    };

    Mode detectModeFromArgv0(const QString& argv0) {
        const QString basename = QFileInfo(argv0).fileName();

        if (basename.contains("pinentry", Qt::CaseInsensitive))
            return Mode::Pinentry;

        if (basename.contains("keyring", Qt::CaseInsensitive))
            return Mode::Keyring;

        return Mode::Cli; // Default to CLI mode (which includes daemon)
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

    int runCli(QCoreApplication& app, const QString& socketPathOverride) {
        QCommandLineParser parser;
        parser.setApplicationDescription("Noctalia Auth - Unified authentication agent");
        parser.addHelpOption();
        parser.addVersionOption();

        // Mode options
        QCommandLineOption optDaemon(QStringList{"daemon", "d"}, "Run as daemon (polkit agent + IPC server).");
        QCommandLineOption optKeyring(QStringList{"keyring"}, "Run as keyring prompter (GCR replacement).");
        QCommandLineOption optPinentry(QStringList{"pinentry"}, "Run as GPG pinentry.");

        // CLI options (for interacting with running daemon)
        QCommandLineOption optPing(QStringList{"ping"}, "Check if the daemon is reachable.");
        QCommandLineOption optNext(QStringList{"next"}, "Fetch the next pending request.");
        QCommandLineOption optRespond(QStringList{"respond"}, "Respond to a request (cookie).", "cookie");
        QCommandLineOption optCancel(QStringList{"cancel"}, "Cancel a request (cookie).", "cookie");
        QCommandLineOption optSocket(QStringList{"socket", "s"}, "Override socket path.", "path");

        parser.addOption(optDaemon);
        parser.addOption(optKeyring);
        parser.addOption(optPinentry);
        parser.addOption(optPing);
        parser.addOption(optNext);
        parser.addOption(optRespond);
        parser.addOption(optCancel);
        parser.addOption(optSocket);

        parser.process(app);

        const QString socketPath = parser.isSet(optSocket) ? parser.value(optSocket) : (!socketPathOverride.isEmpty() ? socketPathOverride : noctalia::socketPath());

        // Check for explicit mode switches
        if (parser.isSet(optDaemon)) {
            return modes::runDaemon(app, socketPath);
        }

        if (parser.isSet(optKeyring)) {
            // Keyring mode is handled separately with GLib
            std::print(stderr, "Keyring mode not yet implemented via --keyring flag\n");
            return 1;
        }

        if (parser.isSet(optPinentry)) {
            return modes::runPinentry();
        }

        // CLI commands for interacting with daemon
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
            QTextStream   stdinStream(stdin);
            const QString password = stdinStream.readLine();
            QJsonObject   response;
            const bool    ok = sendJsonCommand(socketPath, QJsonObject{{"type", "respond"}, {"id", cookie}, {"response", password}}, &response);
            return (ok && response.value("type").toString() == "ok") ? 0 : 1;
        }

        if (parser.isSet(optCancel)) {
            const QString cookie = parser.value(optCancel);
            QJsonObject   response;
            const bool    ok = sendJsonCommand(socketPath, QJsonObject{{"type", "cancel"}, {"id", cookie}}, &response);
            return (ok && response.value("type").toString() == "ok") ? 0 : 1;
        }

        // No explicit mode or CLI command - default to daemon
        return modes::runDaemon(app, socketPath);
    }

} // namespace

int main(int argc, char* argv[]) {
    // Detect mode from argv[0] (symlink detection)
    const QString argv0 = QString::fromLocal8Bit(argv[0]);
    const Mode    mode  = detectModeFromArgv0(argv0);

    switch (mode) {
        case Mode::Pinentry:
            // Pinentry mode doesn't need Qt - it's stdin/stdout based
            return modes::runPinentry();

        case Mode::Keyring:
            // Keyring mode uses GLib main loop, not Qt
            return modes::runKeyring(argc, argv);

        case Mode::Daemon:
        case Mode::Cli:
            // CLI and daemon modes use Qt
            break;
    }

    QCoreApplication app(argc, argv);
    app.setApplicationName("noctalia-auth");
    app.setApplicationVersion("1.0.0");

    return runCli(app, {});
}
