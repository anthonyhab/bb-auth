// bb-auth-intent-hook — PreToolUse/BeforeTool hook for privileged commands,
// auto-detecting the harness from the stdin payload.
//
// Wired per harness (see README.md): Claude Code settings.json, Devin CLI
// hooks.v1.json / .claude/settings.json, Gemini CLI settings.json, Codex CLI
// hooks.json / config.toml. It does two display/audit-only things, both
// fail-open:
//
//   1. Declares WHO + WHY to the bb-auth daemon (`intent.declare`) so the auth
//      prompt can attribute the request and show the reason.
//   2. Rewrites a *simple* leading `sudo CMD` into `pkexec CMD` (via the
//      harness's input-merge channel) so the escalation routes through
//      bb-auth's supervised polkit prompt instead of sudo's unsupervised PAM
//      path.
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

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

namespace {

    constexpr int kDeclareTtlMs = 20000;
    constexpr int kMaxReason = 2000;

    enum class Harness {
        ClaudeCode,
        Codex,
        Devin,
        GeminiCli,
        Unknown
    };

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

    // Codex reuses the Claude-shaped payload but adds its own fields; `turn_id`
    // is documented as codex-specific and Claude Code never sends it. Its shell
    // calls report `tool_name` "Bash" (unified exec may report "exec_command"
    // with "Bash" in `matcher_aliases`), so the discriminator must come from
    // the extra fields, not the tool name.
    Harness detectHarness(const QJsonObject &event) {
        const QString toolName = event.value(QStringLiteral("tool_name")).toString();
        if (!event.value(QStringLiteral("turn_id")).toString().isEmpty()) {
            bool bash = toolName == QStringLiteral("Bash") ||
                        toolName == QStringLiteral("exec_command");
            for (const QJsonValue &alias : event.value(QStringLiteral("matcher_aliases")).toArray())
                bash = bash || alias.toString() == QStringLiteral("Bash");
            return bash ? Harness::Codex : Harness::Unknown;
        }
        if (toolName == QStringLiteral("Bash"))
            return Harness::ClaudeCode;
        if (toolName == QStringLiteral("exec"))
            return Harness::Devin;
        if (toolName == QStringLiteral("run_shell_command"))
            return Harness::GeminiCli;
        return Harness::Unknown;
    }

    QString agentIdFor(Harness h) {
        switch (h) {
            case Harness::ClaudeCode:
                return QStringLiteral("claude-code");
            case Harness::Codex:
                return QStringLiteral("codex");
            case Harness::Devin:
                return QStringLiteral("devin");
            case Harness::GeminiCli:
                return QStringLiteral("gemini-cli");
            case Harness::Unknown:
                return {};
        }
        return {};
    }

    // The most recent assistant prose is our best guess at why the command runs.
    // Only harnesses that expose a transcript path (Claude Code; Devin is
    // Claude-format compatible) can supply this — others get the generic reason.
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

    // Fire the declare to the daemon and wait (bounded) for its reply. Returns
    // true only when the daemon answered `{"type":"ok"}` — proof the supervised
    // path is live. Silent on any failure — the command still runs unchanged.
    // @lat: [[agent-intent#Harness hook channel]]
    bool declareIntent(const QJsonObject &payload) {
        const QByteArray path = socketPath().toLocal8Bit();
        sockaddr_un addr{};
        if (path.size() >= static_cast<int>(sizeof(addr.sun_path)))
            return false;
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return false;
        addr.sun_family = AF_UNIX;
        ::memcpy(addr.sun_path, path.constData(), static_cast<size_t>(path.size()));
        bool ok = false;
        if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
            QByteArray line = QJsonDocument(payload).toJson(QJsonDocument::Compact);
            line.append('\n');
            const ssize_t wrote = ::write(fd, line.constData(), static_cast<size_t>(line.size()));
            if (wrote == line.size()) {
                // Bounded wait: a same-UID squatter that accepts-but-never-replies
                // must not hang the agent's tool call.
                pollfd pfd{fd, POLLIN, 0};
                if (::poll(&pfd, 1, 500) > 0 && (pfd.revents & POLLIN)) {
                    char buf[256];
                    const ssize_t got = ::read(fd, buf, sizeof(buf) - 1);
                    if (got > 0) {
                        buf[got] = '\0';
                        const auto reply = QJsonDocument::fromJson(QByteArray(buf, static_cast<int>(got))).object();
                        ok = reply.value(QStringLiteral("type")).toString() == QStringLiteral("ok");
                    }
                }
            }
        }
        ::close(fd);
        return ok;
    }

    // Rewrites a leading `sudo`/`doas` command into `pkexec`, sharing the
    // aisudo CLI/shim option subset: `-n`/`--non-interactive` drops (the
    // supervised GUI prompt is the non-interactive path), `-u`/`--user` maps
    // to `pkexec --user`, `--` ends options. Other options, env assignments,
    // shell metacharacters, and probe-only invocations decline — they still
    // get intent declared; they just run unchanged.
    // @lat: [[agent-intent#sudo to pkexec rewrite]]
    bool simpleSudoRewrite(const QString &command, QString *rewritten) {
        static const QRegularExpression meta(QStringLiteral("[|&;<>`$()\\n]"));
        static const QRegularExpression leadTok(QStringLiteral("^(\\S+)\\s*"));
        const QString trimmed = command.trimmed();
        const bool    isSudo  = trimmed.startsWith(QStringLiteral("sudo "));
        const bool    isDoas  = trimmed.startsWith(QStringLiteral("doas "));
        if (!isSudo && !isDoas)
            return false;
        if (meta.match(trimmed).hasMatch())
            return false;
        QString rest = trimmed.mid(trimmed.indexOf(QLatin1Char(' '))).trimmed();
        QString user;
        for (;;) {
            const auto m = leadTok.match(rest);
            if (!m.hasMatch())
                return false; // probe / options with no command
            const QString tok = m.captured(1);
            if (tok == QLatin1String("--")) {
                rest = rest.mid(m.capturedLength());
                break;
            }
            if (tok == QLatin1String("-n") ||
                tok == QLatin1String("--non-interactive")) {
                rest = rest.mid(m.capturedLength());
                continue;
            }
            if (tok == QLatin1String("-u") || tok == QLatin1String("--user")) {
                rest = rest.mid(m.capturedLength());
                const auto v = leadTok.match(rest);
                if (!v.hasMatch())
                    return false; // missing user argument
                user = v.captured(1);
                rest = rest.mid(v.capturedLength());
                continue;
            }
            if (tok.startsWith(QStringLiteral("--user="))) {
                user = tok.mid(7);
                rest = rest.mid(m.capturedLength());
                continue;
            }
            if (tok.startsWith(QStringLiteral("-u")) && tok.size() > 2) {
                user = tok.mid(2);
                rest = rest.mid(m.capturedLength());
                continue;
            }
            if (tok.startsWith(QLatin1Char('-')) ||
                tok.contains(QLatin1Char('=')))
                return false; // unsupported option / env assignment
            break;            // first non-option token: command begins
        }
        if (rest.isEmpty())
            return false;
        *rewritten = QStringLiteral("pkexec") +
            (user.isEmpty() ? QString()
                            : QStringLiteral(" --user ") + user) +
            QStringLiteral(" ") + rest;
        return true;
    }

    // A privileged *leading* token only — `cat sudo.conf` or `echo sudo | ...`
    // must not declare: a stray "(no rationale captured)" would clobber a real
    // pending reason (the daemon correlates latest-wins per agent).
    bool isPrivileged(const QString &command) {
        static const QRegularExpression re(QStringLiteral("^\\s*(sudo|pkexec|doas)\\b"));
        return re.match(command).hasMatch();
    }

    // Input-merge output differs per harness: Claude Code, Devin, and Codex
    // merge `updatedInput` (codex requires `permissionDecision: "allow"`
    // alongside it); Gemini CLI merges `hookSpecificOutput.tool_input`.
    void emitRewrite(Harness harness, const QString &rewritten) {
        QJsonObject out;
        if (harness == Harness::GeminiCli) {
            out = QJsonObject{
                {QStringLiteral("hookSpecificOutput"),
                 QJsonObject{{QStringLiteral("tool_input"),
                              QJsonObject{{QStringLiteral("command"), rewritten}}}}},
            };
        } else {
            const QJsonObject hookOut{
                {QStringLiteral("hookEventName"), QStringLiteral("PreToolUse")},
                {QStringLiteral("permissionDecision"), QStringLiteral("allow")},
                {QStringLiteral("permissionDecisionReason"),
                 QStringLiteral("Routed sudo -> pkexec so bb-auth can supervise this escalation.")},
                {QStringLiteral("updatedInput"),
                 QJsonObject{{QStringLiteral("command"), rewritten}}},
            };
            out = QJsonObject{{QStringLiteral("hookSpecificOutput"), hookOut}};
        }
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
    const Harness harness = detectHarness(event);

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

    QJsonObject declare{
        {QStringLiteral("type"), QStringLiteral("intent.declare")},
        {QStringLiteral("reason"), reason},
        {QStringLiteral("command"), rewrite ? rewritten : command},
        {QStringLiteral("cwd"), event.value(QStringLiteral("cwd")).toString()},
        {QStringLiteral("channel"), QStringLiteral("hook")},
        {QStringLiteral("ttlMs"), kDeclareTtlMs},
    };
    const QString agentId = agentIdFor(harness);
    if (!agentId.isEmpty())
        declare.insert(QStringLiteral("agent"), agentId);
    const bool daemonAnswered = declareIntent(declare);

    // Rewrite only when the daemon acknowledged: with no supervised agent,
    // `pkexec` can hard-fail where `sudo` would have worked — keep the command
    // on its original auth path instead (fail-open per ADR-0003).
    if (rewrite && daemonAnswered && harness != Harness::Unknown)
        emitRewrite(harness, rewritten);
    return 0; // always allow; polkit/pkexec remains the gate
}
