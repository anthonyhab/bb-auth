// bb-auth-intent-hook — Claude Code PreToolUse hook for privileged commands.
//
// Gated in settings.json by `if: "Bash(sudo *)"` (and pkexec/doas), so it spawns
// only when an agent is actually escalating privilege — not on every Bash call.
// It does two display/audit-only things, both fail-open:
//
//   1. Declares WHO + WHY to the bb-auth daemon (`intent.declare`) so the auth
//      prompt can attribute the request and show the reason.
//   2. Rewrites a *simple* leading `sudo CMD` into `pkexec CMD` (via the PreToolUse
//      `updatedInput` channel) so the escalation routes through bb-auth's supervised
//      polkit prompt instead of sudo's unsupervised PAM path.
//
// Trust: the reason and agent id are self-asserted and NEVER gate the decision —
// that stays with polkit and the daemon's OS-resolved process identity. Anything
// that goes wrong leaves the command unchanged and lets it run. See docs/adr/0003.

#include <QByteArray>
#include <QChar>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

namespace {

    constexpr int kDeclareTtlMs = 20000;
    constexpr int kMaxReason = 2000;

    QByteArray readAllStdin() {
        QByteArray data;
        char buf[4096];
        ssize_t n;
        while ((n = ::read(STDIN_FILENO, buf, sizeof(buf))) > 0)
            data.append(buf, static_cast<int>(n));
        return data;
    }

    QString socketPath() {
        if (const char *runtime = ::getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
            return QString::fromLocal8Bit(runtime) + QStringLiteral("/bb-auth.sock");
        return QStringLiteral("/run/user/%1/bb-auth.sock").arg(::getuid());
    }

    // The most recent assistant prose is our best guess at why the command runs.
    QString lastAssistantText(const QString &transcriptPath) {
        if (transcriptPath.isEmpty())
            return {};
        QFile file(transcriptPath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return {};
        QString text;
        while (!file.atEnd()) {
            const QByteArray line = file.readLine().trimmed();
            if (line.isEmpty())
                continue;
            const QJsonObject entry = QJsonDocument::fromJson(line).object();
            const QJsonObject msg = entry.contains(QStringLiteral("message"))
                ? entry.value(QStringLiteral("message")).toObject()
                : entry;
            if (msg.value(QStringLiteral("role")).toString() != QStringLiteral("assistant"))
                continue;
            const QJsonValue content = msg.value(QStringLiteral("content"));
            if (content.isString()) {
                const QString s = content.toString().trimmed();
                if (!s.isEmpty())
                    text = s;
            } else if (content.isArray()) {
                QStringList parts;
                for (const QJsonValue &block : content.toArray()) {
                    const QJsonObject b = block.toObject();
                    if (b.value(QStringLiteral("type")).toString() == QStringLiteral("text")) {
                        const QString t = b.value(QStringLiteral("text")).toString().trimmed();
                        if (!t.isEmpty())
                            parts << t;
                    }
                }
                const QString joined = parts.join(QStringLiteral(" "));
                if (!joined.isEmpty())
                    text = joined;
            }
        }
        return text.left(kMaxReason);
    }

    // Fire the declare to the daemon and drain one reply (so it can resolve our peer
    // creds before we close). Silent on any failure — the command still runs.
    void declareIntent(const QJsonObject &payload) {
        const QByteArray path = socketPath().toLocal8Bit();
        sockaddr_un addr{};
        if (path.size() >= static_cast<int>(sizeof(addr.sun_path)))
            return;
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return;
        addr.sun_family = AF_UNIX;
        ::memcpy(addr.sun_path, path.constData(), static_cast<size_t>(path.size()));
        if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
            QByteArray line = QJsonDocument(payload).toJson(QJsonDocument::Compact);
            line.append('\n');
            const ssize_t wrote = ::write(fd, line.constData(), static_cast<size_t>(line.size()));
            (void) wrote;
            char buf[256];
            const ssize_t got = ::read(fd, buf, sizeof(buf));
            (void) got;
        }
        ::close(fd);
    }

    // True only for a clean, simple leading `sudo` command — no options, no env
    // assignments, no shell metacharacters. We deliberately decline anything
    // compound or option-bearing: pkexec's flags and minimal environment differ
    // from sudo's, so rewriting those could change behaviour. They still get intent
    // declared; they just run unchanged.
    bool simpleSudoRewrite(const QString &command, QString *rewritten) {
        static const QRegularExpression meta(QStringLiteral("[|&;<>`$()\\n]"));
        const QString trimmed = command.trimmed();
        if (!trimmed.startsWith(QStringLiteral("sudo ")))
            return false;
        if (meta.match(trimmed).hasMatch())
            return false;
        const QString rest = trimmed.mid(5).trimmed(); // after "sudo "
        if (rest.isEmpty() || rest.startsWith(QLatin1Char('-')))
            return false; // sudo options (-i, -u, -E, ...) don't translate cleanly
        if (rest.section(QLatin1Char(' '), 0, 0).contains(QLatin1Char('=')))
            return false; // `sudo VAR=x cmd` env assignment is sudo-specific
        *rewritten = QStringLiteral("pkexec ") + rest;
        return true;
    }

    bool isPrivileged(const QString &command) {
        static const QRegularExpression re(QStringLiteral("\\b(sudo|pkexec|doas)\\b"));
        return re.match(command).hasMatch();
    }

    void emitRewrite(const QString &rewritten) {
        const QJsonObject hookOut{
            {QStringLiteral("hookEventName"), QStringLiteral("PreToolUse")},
            {QStringLiteral("permissionDecision"), QStringLiteral("allow")},
            {QStringLiteral("permissionDecisionReason"),
             QStringLiteral("Routed sudo -> pkexec so bb-auth can supervise this escalation.")},
            {QStringLiteral("updatedInput"),
             QJsonObject{{QStringLiteral("command"), rewritten}}},
        };
        const QJsonObject out{{QStringLiteral("hookSpecificOutput"), hookOut}};
        const QByteArray json = QJsonDocument(out).toJson(QJsonDocument::Compact);
        const ssize_t wrote = ::write(STDOUT_FILENO, json.constData(), static_cast<size_t>(json.size()));
        (void) wrote;
    }

} // namespace

int main() {
    const QByteArray input = readAllStdin();
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(input, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject())
        return 0; // malformed input: do nothing, allow

    const QJsonObject event = doc.object();
    if (event.value(QStringLiteral("tool_name")).toString() != QStringLiteral("Bash"))
        return 0;

    const QString command = event.value(QStringLiteral("tool_input"))
                                .toObject()
                                .value(QStringLiteral("command"))
                                .toString();
    if (command.isEmpty() || !isPrivileged(command))
        return 0;

    QString reason = lastAssistantText(event.value(QStringLiteral("transcript_path")).toString());
    if (reason.isEmpty())
        reason = QStringLiteral("(no rationale captured)");

    QString rewritten;
    const bool rewrite = simpleSudoRewrite(command, &rewritten);

    declareIntent(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("intent.declare")},
        {QStringLiteral("reason"), reason},
        {QStringLiteral("agent"), QStringLiteral("claude-code")},
        {QStringLiteral("command"), rewrite ? rewritten : command},
        {QStringLiteral("cwd"), event.value(QStringLiteral("cwd")).toString()},
        {QStringLiteral("channel"), QStringLiteral("hook")},
        {QStringLiteral("ttlMs"), kDeclareTtlMs},
    });

    if (rewrite)
        emitRewrite(rewritten);
    return 0; // always allow; polkit/pkexec remains the gate
}
