#include "Common.hpp"

#include "../../src/common/ProcAgent.hpp"

#include <QJsonDocument>
#include <QJsonValue>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

extern char **environ;

namespace bb::agent {

    namespace {

        qint64 nowMs() {
            timespec ts{};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            return static_cast<qint64>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
        }

        // One JSON line out, one JSON line back, within `budgetMs` total. A
        // same-UID squatter that accepts but never replies must not hang the
        // agent's tool call.
        QByteArray roundTrip(const QByteArray &line, int budgetMs) {
            const QByteArray path = socketPath().toLocal8Bit();
            sockaddr_un      addr{};
            if (path.size() >= static_cast<int>(sizeof(addr.sun_path)))
                return {};
            const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd < 0)
                return {};
            addr.sun_family = AF_UNIX;
            std::memcpy(addr.sun_path, path.constData(), static_cast<size_t>(path.size()));
            QByteArray reply;
            if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0 &&
                ::write(fd, line.constData(), static_cast<size_t>(line.size())) == line.size()) {
                const qint64 deadline = nowMs() + budgetMs;
                while (!reply.contains('\n') && reply.size() <= 4096) {
                    const qint64 left = deadline - nowMs();
                    if (left <= 0)
                        break;
                    pollfd pfd{fd, POLLIN, 0};
                    const int pr = ::poll(&pfd, 1, static_cast<int>(left));
                    if (pr < 0 && errno == EINTR)
                        continue;
                    if (pr <= 0)
                        break;
                    char          buf[512];
                    const ssize_t got = ::read(fd, buf, sizeof(buf));
                    if (got <= 0)
                        break;
                    reply.append(buf, static_cast<int>(got));
                }
            }
            ::close(fd);
            const qsizetype nl = reply.indexOf('\n');
            return nl < 0 || reply.size() > 4096 ? QByteArray() : reply.left(nl);
        }

        std::vector<std::string> toStd(const QStringList &argv) {
            std::vector<std::string> out;
            out.reserve(static_cast<size_t>(argv.size()));
            for (const QString &a : argv)
                out.push_back(a.toStdString());
            return out;
        }

        std::vector<char *> cArgv(std::vector<std::string> &args) {
            std::vector<char *> out;
            for (auto &a : args)
                out.push_back(a.data());
            out.push_back(nullptr);
            return out;
        }

        QString envOr(const char *name, const char *configured, const QString &fallback) {
            if (const char *v = std::getenv(name); v && *v)
                return QString::fromLocal8Bit(v);
            return configured[0] == '@' ? fallback : QString::fromUtf8(configured);
        }

        QString selfDir() {
            char          buf[4096];
            const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
            if (n <= 0)
                return {};
            const QString exe = QString::fromLocal8Bit(buf, static_cast<int>(n));
            return exe.left(exe.lastIndexOf(QLatin1Char('/')));
        }

    } // namespace

    QString socketPath() {
        if (const char *runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
            return QString::fromLocal8Bit(runtime) + QStringLiteral("/bb-auth.sock");
        return QStringLiteral("/run/user/%1/bb-auth.sock").arg(::getuid());
    }

    DeclareResult declareIntent(const QJsonObject &payload) {
        QByteArray line = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        line.append('\n');
        const QJsonObject reply = QJsonDocument::fromJson(roundTrip(line, 500)).object();
        DeclareResult     r;
        if (reply.value(QStringLiteral("type")).toString() == QStringLiteral("ok") &&
            reply.value(QStringLiteral("bound")).isBool()) {
            r.answered = true;
            r.bound    = reply.value(QStringLiteral("bound")).toBool();
        }
        return r;
    }

    bool daemonPing() {
        const QJsonObject reply = QJsonDocument::fromJson(roundTrip("{\"type\":\"ping\"}\n", 500)).object();
        return reply.value(QStringLiteral("type")).toString() == QStringLiteral("pong");
    }

    // @lat: [[agent-intent#Challenge-gated approval]]
    int pkcheckExec() {
        std::vector<std::string> args{"pkcheck", "--action-id", "org.freedesktop.policykit.exec",
                                      "--process", std::to_string(::getpid())};
        auto                     argv = cArgv(args);

        posix_spawn_file_actions_t fa;
        if (posix_spawn_file_actions_init(&fa) != 0)
            return -1;
        // stdout may be the harness channel — pkcheck's "polkit\56result=..." must not leak.
        for (int fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO})
            posix_spawn_file_actions_addopen(&fa, fd, "/dev/null", fd == STDIN_FILENO ? O_RDONLY : O_WRONLY, 0);
        pid_t     child = -1;
        const int rc    = posix_spawnp(&child, "pkcheck", &fa, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        if (rc != 0)
            return -1;
        int status = 0;
        for (int waited = 0; waited < 1000; waited += 5) {
            const pid_t r = ::waitpid(child, &status, WNOHANG);
            if (r == child)
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            if (r < 0)
                return -1;
            ::usleep(5000);
        }
        ::kill(child, SIGKILL);
        ::waitpid(child, &status, 0);
        return -1;
    }

    bool polkitWillChallenge() {
        return pkcheckExec() == 2;
    }

    QString detectAgentAncestor() {
        // Test hook: pin human mode regardless of the ambient process tree (the
        // suite may itself run under an agent). Harmless — the shim is
        // supervision UX, not a security boundary.
        if (const char *v = std::getenv("BB_AUTH_TEST_ASSUME_HUMAN"); v && *v == '1')
            return {};
        const qint64 uid = ::getuid();
        qint64       pid = ::getppid();
        for (int hop = 0; hop < 16 && pid > 1; ++hop) {
            const auto proc = bb::readProc(pid);
            if (!proc || proc->uid != uid)
                break;
            if (const AgentMatch m = bb::detectAgent(*proc); m.isValid())
                return m.kind;
            if (proc->ppid <= 1 || proc->ppid == pid)
                break;
            pid = proc->ppid;
        }
        return {};
    }

    void writeOut(const QByteArray &bytes) {
        const ssize_t n = ::write(STDOUT_FILENO, bytes.constData(), static_cast<size_t>(bytes.size()));
        (void) n;
    }

    void writeErr(const QByteArray &bytes) {
        const ssize_t n = ::write(STDERR_FILENO, bytes.constData(), static_cast<size_t>(bytes.size()));
        (void) n;
    }

    void execPath(const QString &path, const QStringList &argv) {
        auto              args = toStd(argv);
        auto              c    = cArgv(args);
        const std::string p    = path.toStdString();
        ::execv(p.c_str(), c.data());
    }

    void execSearch(const QStringList &argv) {
        auto args = toStd(argv);
        auto c    = cArgv(args);
        ::execvp(c[0], c.data());
    }

    QString hookPath() {
        return envOr("BB_AUTH_INTENT_HOOK", BB_AUTH_INTENT_HOOK_PATH, selfDir() + QStringLiteral("/bb-auth-intent-hook"));
    }

    QString integrationsDir() {
        return envOr("BB_AUTH_INTEGRATIONS_DIR", BB_AUTH_INTEGRATIONS_DIR, selfDir() + QStringLiteral("/../share/bb-auth/integrations"));
    }

    QString shimDir() {
        return envOr("BB_AUTH_SHIM_DIR", BB_AUTH_SHIM_DIR, selfDir() + QStringLiteral("/bb-auth-shims"));
    }

} // namespace bb::agent
