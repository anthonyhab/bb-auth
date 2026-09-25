// bb-auth-agent — one binary, dispatched by argv[0]:
//
//   bb-auth-intent-hook        harness hook (stdin JSON)       Hook.cpp
//   aisudo                     voluntary declare + exec CLI    Aisudo.cpp
//   sudo | doas | pkexec       agent-gated PATH shim           Aisudo.cpp
//   bb-auth-agents             harness installer               Installer.cpp
//
// `bb-auth-agent <mode> ...` selects a mode explicitly (hook, aisudo,
// agents). One compiled binary replaces the former Python tools: no
// interpreter on the sudo path, one version, one install.

#include "Common.hpp"

#include <QFileInfo>
#include <QStringList>

#include <cstring>

namespace bb::agent {
    int runHook();
    int runAisudo(const QStringList &argv);
    int runShim(const QString &tool, const QStringList &args);
    int runAgents(const QStringList &argv);
} // namespace bb::agent

namespace {

    QStringList argsFrom(int argc, char **argv, int first) {
        QStringList out;
        for (int i = first; i < argc; ++i)
            out << QString::fromLocal8Bit(argv[i]);
        return out;
    }

    int dispatch(const QString &mode, int argc, char **argv, int first) {
        using namespace bb::agent;
        if (mode == QLatin1String("bb-auth-intent-hook") || mode == QLatin1String("hook")) {
            if (first < argc && std::strcmp(argv[first], "--version") == 0) {
                writeOut("bb-auth-intent-hook " BB_AUTH_VERSION "\n");
                return 0;
            }
            return runHook();
        }
        if (mode == QLatin1String("aisudo"))
            return runAisudo(argsFrom(argc, argv, first));
        if (mode == QLatin1String("bb-auth-agents") || mode == QLatin1String("agents"))
            return runAgents(argsFrom(argc, argv, first));
        if (mode == QLatin1String("sudo") || mode == QLatin1String("doas") || mode == QLatin1String("pkexec"))
            return runShim(mode, argsFrom(argc, argv, first));
        return -1;
    }

} // namespace

int main(int argc, char **argv) {
    // No QCoreApplication: nothing here needs an event loop, and a sudo shim
    // must never let Qt interpret its argv.
    const QString self = QFileInfo(QString::fromLocal8Bit(argv[0])).fileName();
    if (const int rc = dispatch(self, argc, argv, 1); rc >= 0)
        return rc;
    if (argc > 1)
        if (const int rc = dispatch(QString::fromLocal8Bit(argv[1]), argc, argv, 2); rc >= 0)
            return rc;
    bb::agent::writeErr("usage: bb-auth-agent {hook|aisudo|agents} ...\n"
                        "  (or invoke as bb-auth-intent-hook, aisudo, bb-auth-agents, sudo/doas/pkexec)\n");
    return 2;
}
