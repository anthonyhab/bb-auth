#include "FallbackClient.hpp"
#include "FallbackWindow.hpp"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLockFile>

#include <cstdlib>

int main(int argc, char* argv[]) {
    const QByteArray waylandDisplay = qgetenv("WAYLAND_DISPLAY");
    const QByteArray platformEnv    = qgetenv("QT_QPA_PLATFORM");
    if (!waylandDisplay.isEmpty() && platformEnv.isEmpty()) {
        // Prefer native Wayland when available; keep xcb fallback for mixed setups.
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("wayland;xcb"));
    }

    QApplication app(argc, argv);
    app.setApplicationName("bb-auth-fallback");
    QGuiApplication::setDesktopFileName("bb-auth-fallback");
    qWarning() << "bb-auth-fallback platform:" << QGuiApplication::platformName() << "marker:geo-debug-v2";

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption socketOpt(QStringList{"socket", "s"}, "Override socket path", "path");
    parser.addOption(socketOpt);
    parser.process(app);

    const QString runtimeDir    = qEnvironmentVariable("XDG_RUNTIME_DIR");
    const QString defaultSocket = runtimeDir.isEmpty() ? QString() : runtimeDir + "/bb-auth.sock";
    const QString socketPath    = parser.isSet(socketOpt) ? parser.value(socketOpt) : defaultSocket;

    if (socketPath.isEmpty()) {
        return 1;
    }

    const QString lockPath = QFileInfo(socketPath).absolutePath() + "/bb-auth-fallback.lock";
    QLockFile     fallbackLock(lockPath);
    if (!fallbackLock.tryLock(0)) {
        return 0;
    }

    bb::FallbackClient client(socketPath);
    bb::FallbackWindow window(&client);
    client.start();
    const int rc = app.exec();
    fallbackLock.unlock();
    std::_Exit(rc);
}
