#include "FallbackClient.hpp"
#include "FallbackWindow.hpp"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLockFile>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("noctalia-auth-fallback");
    QGuiApplication::setDesktopFileName("noctalia-auth-fallback");

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption socketOpt(QStringList{"socket", "s"}, "Override socket path", "path");
    parser.addOption(socketOpt);
    parser.process(app);

    const QString runtimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    const QString defaultSocket = runtimeDir.isEmpty() ? QString() : runtimeDir + "/noctalia-auth.sock";
    const QString socketPath = parser.isSet(socketOpt) ? parser.value(socketOpt) : defaultSocket;

    if (socketPath.isEmpty()) {
        return 1;
    }

    const QString lockPath = QFileInfo(socketPath).absolutePath() + "/noctalia-auth-fallback.lock";
    QLockFile fallbackLock(lockPath);
    if (!fallbackLock.tryLock(0)) {
        return 0;
    }

    noctalia::FallbackClient client(socketPath);
    noctalia::FallbackWindow window(&client);
    client.start();

    return app.exec();
}
