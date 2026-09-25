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

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

extern char **environ;

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

    // Assistant prose carried by one transcript JSONL line, or empty.
    QString assistantText(const QByteArray &line) {
        if (line.isEmpty())
            return {};
        const QJsonObject entry = QJsonDocument::fromJson(line).object();
        const QJsonObject msg = entry.contains(QStringLiteral("message"))
            ? entry.value(QStringLiteral("message")).toObject()
            : entry;
        if (msg.value(QStringLiteral("role")).toString() != QStringLiteral("assistant"))
            return {};
        const QJsonValue content = msg.value(QStringLiteral("content"));
        if (content.isString())
            return content.toString().trimmed();
        QStringList parts;
        for (const QJsonValue &block : content.toArray()) {
            const QJsonObject b = block.toObject();
            if (b.value(QStringLiteral("type")).toString() == QStringLiteral("text")) {
                const QString t = b.value(QStringLiteral("text")).toString().trimmed();
                if (!t.isEmpty())
                    parts << t;
            }
        }
        return parts.join(QStringLiteral(" "));
    }

    // The most recent assistant prose is our best guess at why the command runs.
    // Only harnesses that expose a transcript path (Claude Code; Devin is
    // Claude-format compatible; codex) can supply this — others get the generic
    // reason. Scans backwards in chunks: transcripts grow to many MiB and the
    // answer is almost always in the last few lines.
    QString lastAssistantText(const QString &transcriptPath) {
        constexpr qint64 kChunk   = 64 * 1024;
        constexpr qint64 kMaxScan = 8 * 1024 * 1024;
        if (transcriptPath.isEmpty())
            return {};
        QFile file(transcriptPath);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        qint64     pos     = file.size();
        qint64     scanned = 0;
        QByteArray carry; // partial first line of the region already read
        while (pos > 0 && scanned < kMaxScan) {
            const qint64 n = qMin(kChunk, pos);
            pos -= n;
            scanned += n;
            if (!file.seek(pos))
                return {};
            QList<QByteArray> lines = (file.read(n) + carry).split('\n');
            carry = pos > 0 ? lines.takeFirst() : QByteArray();
            for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
                const QString text = assistantText(it->trimmed());
                if (!text.isEmpty())
                    return text.left(kMaxReason);
            }
        }
        return {};
    }

    // True only when polkit would *challenge* the user for pkexec (pkcheck
    // exit 2, no interaction allowed). The rewrite answers the harness with
    // "allow", which skips its own prompt and auto mode classifier — safe only
    // if a human will really see the polkit prompt. A YES rule (e.g.
    // empower.rules), a denial, an error, or a missing pkcheck all return
    // false and keep the harness gate. Subject is this process: same uid and
    // session as the pkexec that follows. Bounded so a wedged polkitd cannot
    // stall the tool call.
    // @lat: [[agent-intent#Challenge-gated approval]]
    bool polkitWillChallenge() {
        const std::string pid = std::to_string(::getpid());
        char              arg0[] = "pkcheck", arg1[] = "--action-id",
                          arg2[] = "org.freedesktop.policykit.exec", arg3[] = "--process";
        std::string       arg4   = pid;
        char *const       argv[] = {arg0, arg1, arg2, arg3, arg4.data(), nullptr};

        posix_spawn_file_actions_t fa;
        if (posix_spawn_file_actions_init(&fa) != 0)
            return false;
        // stdout is the harness channel — pkcheck's "polkit\56result=..." must not leak.
        for (int fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO})
            posix_spawn_file_actions_addopen(&fa, fd, "/dev/null", fd == STDIN_FILENO ? O_RDONLY : O_WRONLY, 0);
        pid_t     child = -1;
        const int rc    = posix_spawnp(&child, "pkcheck", &fa, nullptr, argv, environ);
        posix_spawn_file_actions_destroy(&fa);
        if (rc != 0)
            return false;
        int status = 0;
        for (int waited = 0; waited < 1000; waited += 5) {
            const pid_t r = ::waitpid(child, &status, WNOHANG);
            if (r == child)
                return WIFEXITED(status) && WEXITSTATUS(status) == 2;
            if (r < 0)
                return false;
            ::usleep(5000);
        }
        ::kill(child, SIGKILL);
        ::waitpid(child, &status, 0);
        return false;
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

    bool isPkexec(const QString &command) {
        static const QRegularExpression re(QStringLiteral("^\\s*pkexec\\b"));
        return re.match(command).hasMatch();
    }

    const QString kHandoffContext = QStringLiteral(
        "bb-auth routed this command to pkexec: the user approves it at a bb-auth "
        "authentication prompt on their desktop, not in this terminal. Exit status 126 "
        "means they dismissed the prompt and 127 means authorization failed. Either way "
        "the user declined — do not retry through sudo, sudo -S, SUDO_ASKPASS, su, run0, "
        "doas, or by editing sudoers or polkit rules; tell the user what you needed.");

    void emitJson(const QJsonObject &out) {
        const QByteArray json  = QJsonDocument(out).toJson(QJsonDocument::Compact);
        const ssize_t    wrote = ::write(STDOUT_FILENO, json.constData(), static_cast<size_t>(json.size()));
        (void) wrote;
    }

    void emitHookOutput(const QString &event, const QString &key, const QString &value) {
        emitJson(QJsonObject{{QStringLiteral("hookSpecificOutput"),
                              QJsonObject{{QStringLiteral("hookEventName"), event}, {key, value}}}});
    }

    // Claude Code post events for a pkexec call: tell the auto mode classifier
    // how polkit authorized a success, and tell the model a 126/127 failure is
    // a human decline. Never relays tool output; never declares.
    // @lat: [[agent-intent#Post-run annotation]]
    void handlePostEvent(const QString &eventName, const QJsonObject &event) {
        if (eventName == QStringLiteral("PostToolUse")) {
            // No retained authorization exists for auth_admin actions, so a
            // still-challenging pkcheck proves the run authenticated.
            const QString note = polkitWillChallenge()
                ? QStringLiteral("bb-auth: polkit authorized this pkexec command after the user "
                                 "authenticated at the bb-auth prompt.")
                : QStringLiteral("bb-auth: polkit authorized this pkexec command without an "
                                 "authentication prompt (a polkit rule allowed it).");
            emitHookOutput(eventName, QStringLiteral("classifierContext"), note);
            return;
        }
        const QString firstLine = event.value(QStringLiteral("error")).toString().section(QLatin1Char('\n'), 0, 0).trimmed();
        if (firstLine == QStringLiteral("Exit code 126"))
            emitHookOutput(eventName, QStringLiteral("additionalContext"),
                           QStringLiteral("bb-auth: the user dismissed the authentication prompt for this "
                                          "pkexec command. Treat it as the user declining: do not retry "
                                          "through sudo, sudo -S, SUDO_ASKPASS, su, run0, doas, or by "
                                          "editing sudoers or polkit rules. Tell the user what you needed "
                                          "and let them decide."));
        else if (firstLine == QStringLiteral("Exit code 127"))
            emitHookOutput(eventName, QStringLiteral("additionalContext"),
                           QStringLiteral("bb-auth: pkexec exited 127 — authorization failed (wrong "
                                          "password or not authorized) or the program could not be run. "
                                          "If authorization failed, do not route around it through sudo, "
                                          "su, run0, doas, or sudoers/polkit edits; ask the user."));
    }

    // Input-merge output differs per harness: Claude Code, Devin, and Codex
    // take `updatedInput` (codex requires `permissionDecision: "allow"`
    // alongside it); Gemini CLI takes `hookSpecificOutput.tool_input`. Claude
    // replaces the *whole* input with it, so the original fields (timeout,
    // run_in_background, description) are carried over with only `command`
    // swapped.
    void emitRewrite(Harness harness, QJsonObject toolInput, const QString &rewritten) {
        toolInput.insert(QStringLiteral("command"), rewritten);
        QJsonObject out;
        if (harness == Harness::GeminiCli) {
            out = QJsonObject{
                {QStringLiteral("hookSpecificOutput"),
                 QJsonObject{{QStringLiteral("tool_input"), toolInput}}},
            };
        } else {
            const QJsonObject hookOut{
                {QStringLiteral("hookEventName"), QStringLiteral("PreToolUse")},
                {QStringLiteral("permissionDecision"), QStringLiteral("allow")},
                {QStringLiteral("permissionDecisionReason"),
                 QStringLiteral("Routed sudo -> pkexec so bb-auth can supervise this escalation.")},
                {QStringLiteral("updatedInput"), toolInput},
                {QStringLiteral("additionalContext"), kHandoffContext},
            };
            out = QJsonObject{{QStringLiteral("hookSpecificOutput"), hookOut}};
        }
        emitJson(out);
    }

} // namespace

int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
        const char version[] = "bb-auth-intent-hook " BB_AUTH_VERSION "\n";
        const ssize_t wrote  = ::write(STDOUT_FILENO, version, sizeof(version) - 1);
        (void) wrote;
        return 0;
    }

    const QByteArray input = readAllStdin();
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(input, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject())
        return 0; // malformed input: do nothing, allow

    const QJsonObject event = doc.object();
    const Harness harness = detectHarness(event);

    const QJsonObject toolInput = event.value(QStringLiteral("tool_input")).toObject();
    const QString     command   = toolInput.value(QStringLiteral("command")).toString();
    if (command.isEmpty() || !isPrivileged(command))
        return 0;

    const QString eventName = event.value(QStringLiteral("hook_event_name")).toString();
    if (eventName == QStringLiteral("PostToolUse") || eventName == QStringLiteral("PostToolUseFailure")) {
        if (harness == Harness::ClaudeCode && isPkexec(command))
            handlePostEvent(eventName, event);
        return 0;
    }
    if (!eventName.isEmpty() && eventName != QStringLiteral("PreToolUse") &&
        eventName != QStringLiteral("BeforeTool"))
        return 0; // wired to an event we do not handle

    QString reason = lastAssistantText(event.value(QStringLiteral("transcript_path")).toString());
    if (reason.isEmpty())
        reason = QStringLiteral("(no rationale captured)");

    // Rewrite only when polkit will actually challenge the user — see
    // polkitWillChallenge. Checked before declaring so the audit `command`
    // matches what will run.
    QString rewritten;
    const bool rewrite = harness != Harness::Unknown &&
        simpleSudoRewrite(command, &rewritten) && polkitWillChallenge();

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
    if (rewrite && daemonAnswered)
        emitRewrite(harness, toolInput, rewritten);
    return 0; // always allow; polkit/pkexec remains the gate
}
