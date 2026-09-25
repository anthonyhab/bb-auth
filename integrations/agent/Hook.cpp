// Hook mode (argv0 `bb-auth-intent-hook`): PreToolUse/BeforeTool hook for
// privileged commands, auto-detecting the harness from the stdin payload.
//
//   1. Declares WHO + WHY to the daemon so the prompt can attribute the
//      request and show the reason.
//   2. When polkit will challenge the user, rewrites a clean leading
//      `sudo CMD` into `pkexec CMD` via the harness's input-merge channel and
//      hands the decision to the human at the bb-auth prompt.
//   3. For Claude Code post events on pkexec, annotates the result.
//
// Trust: reason and agent id are self-asserted and never gate the decision;
// anything that goes wrong leaves the command unchanged (ADR 0003).

#include "Common.hpp"
#include "Translate.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QList>
#include <QRegularExpression>

#include <unistd.h>

namespace bb::agent {

    namespace {

        enum class Harness {
            ClaudeCode,
            Codex,
            Devin,
            GeminiCli,
            Unknown
        };

        const QString kHandoffContext = QStringLiteral(
            "bb-auth routed this command to pkexec: the user approves it at a bb-auth "
            "authentication prompt on their desktop, not in this terminal. Exit status 126 "
            "means they dismissed the prompt and 127 means authorization failed. Either way "
            "the user declined — do not retry through sudo, sudo -S, SUDO_ASKPASS, su, run0, "
            "doas, or by editing sudoers or polkit rules; tell the user what you needed.");

        QByteArray readAllStdin() {
            QByteArray data;
            char       buf[4096];
            ssize_t    n;
            while ((n = ::read(STDIN_FILENO, buf, sizeof(buf))) > 0)
                data.append(buf, static_cast<int>(n));
            return data;
        }

        // Codex reuses the Claude-shaped payload but adds `turn_id`, which
        // Claude Code never sends; its shell calls report tool_name "Bash"
        // (unified exec may report "exec_command" with "Bash" in
        // matcher_aliases), so the discriminator must come from extra fields.
        Harness detectHarness(const QJsonObject &event) {
            const QString toolName = event.value(QStringLiteral("tool_name")).toString();
            if (!event.value(QStringLiteral("turn_id")).toString().isEmpty()) {
                bool bash = toolName == QLatin1String("Bash") || toolName == QLatin1String("exec_command");
                for (const QJsonValue &alias : event.value(QStringLiteral("matcher_aliases")).toArray())
                    bash = bash || alias.toString() == QLatin1String("Bash");
                return bash ? Harness::Codex : Harness::Unknown;
            }
            if (toolName == QLatin1String("Bash"))
                return Harness::ClaudeCode;
            if (toolName == QLatin1String("exec"))
                return Harness::Devin;
            if (toolName == QLatin1String("run_shell_command"))
                return Harness::GeminiCli;
            return Harness::Unknown;
        }

        QString agentIdFor(Harness h) {
            switch (h) {
                case Harness::ClaudeCode: return QStringLiteral("claude-code");
                case Harness::Codex: return QStringLiteral("codex");
                case Harness::Devin: return QStringLiteral("devin");
                case Harness::GeminiCli: return QStringLiteral("gemini-cli");
                case Harness::Unknown: return {};
            }
            return {};
        }

        QString assistantText(const QByteArray &line) {
            if (line.isEmpty())
                return {};
            const QJsonObject entry = QJsonDocument::fromJson(line).object();
            const QJsonObject msg   = entry.contains(QStringLiteral("message")) ? entry.value(QStringLiteral("message")).toObject() : entry;
            if (msg.value(QStringLiteral("role")).toString() != QLatin1String("assistant"))
                return {};
            const QJsonValue content = msg.value(QStringLiteral("content"));
            if (content.isString())
                return content.toString().trimmed();
            QStringList parts;
            for (const QJsonValue &block : content.toArray()) {
                const QJsonObject b = block.toObject();
                if (b.value(QStringLiteral("type")).toString() == QLatin1String("text")) {
                    const QString t = b.value(QStringLiteral("text")).toString().trimmed();
                    if (!t.isEmpty())
                        parts << t;
                }
            }
            return parts.join(QLatin1Char(' '));
        }

        // The most recent assistant prose is our best guess at why the command
        // runs. Scans backwards in chunks: transcripts grow to many MiB and the
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
                carry                   = pos > 0 ? lines.takeFirst() : QByteArray();
                for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
                    const QString text = assistantText(it->trimmed());
                    if (!text.isEmpty())
                        return text.left(kMaxReason);
                }
            }
            return {};
        }

        // A privileged *leading* token only — `cat sudo.conf` must not declare:
        // a stray "(no rationale captured)" would clobber a real pending reason
        // (the daemon correlates latest-wins per agent).
        bool isPrivileged(const QString &command) {
            static const QRegularExpression re(QStringLiteral("^\\s*(sudo|pkexec|doas)\\b"));
            return re.match(command).hasMatch();
        }

        bool isPkexec(const QString &command) {
            static const QRegularExpression re(QStringLiteral("^\\s*pkexec\\b"));
            return re.match(command).hasMatch();
        }

        void emitJson(const QJsonObject &out) {
            writeOut(QJsonDocument(out).toJson(QJsonDocument::Compact));
        }

        void emitHookOutput(const QString &event, const QString &key, const QString &value) {
            emitJson(QJsonObject{{QStringLiteral("hookSpecificOutput"),
                                  QJsonObject{{QStringLiteral("hookEventName"), event}, {key, value}}}});
        }

        // Claude Code replaces the *whole* tool input with updatedInput, so
        // the original fields (timeout, run_in_background, description) are
        // carried over with only `command` swapped. Gemini takes
        // hookSpecificOutput.tool_input; Codex requires permissionDecision
        // "allow" beside updatedInput.
        void emitRewrite(Harness harness, QJsonObject toolInput, const QString &rewritten) {
            toolInput.insert(QStringLiteral("command"), rewritten);
            if (harness == Harness::GeminiCli) {
                emitJson(QJsonObject{{QStringLiteral("hookSpecificOutput"), QJsonObject{{QStringLiteral("tool_input"), toolInput}}}});
                return;
            }
            emitJson(QJsonObject{{QStringLiteral("hookSpecificOutput"),
                                  QJsonObject{
                                      {QStringLiteral("hookEventName"), QStringLiteral("PreToolUse")},
                                      {QStringLiteral("permissionDecision"), QStringLiteral("allow")},
                                      {QStringLiteral("permissionDecisionReason"),
                                       QStringLiteral("Routed sudo -> pkexec so bb-auth can supervise this escalation.")},
                                      {QStringLiteral("updatedInput"), toolInput},
                                      {QStringLiteral("additionalContext"), kHandoffContext},
                                  }}});
        }

        // @lat: [[agent-intent#Post-run annotation]]
        void handlePostEvent(const QString &eventName, const QJsonObject &event) {
            if (eventName == QLatin1String("PostToolUse")) {
                // auth_admin keeps no authorization, so a still-challenging
                // pkcheck proves the run authenticated.
                const QString note = polkitWillChallenge()
                    ? QStringLiteral("bb-auth: polkit authorized this pkexec command after the user "
                                     "authenticated at the bb-auth prompt.")
                    : QStringLiteral("bb-auth: polkit authorized this pkexec command without an "
                                     "authentication prompt (a polkit rule allowed it).");
                emitHookOutput(eventName, QStringLiteral("classifierContext"), note);
                return;
            }
            const QString firstLine = event.value(QStringLiteral("error")).toString().section(QLatin1Char('\n'), 0, 0).trimmed();
            if (firstLine == QLatin1String("Exit code 126"))
                emitHookOutput(eventName, QStringLiteral("additionalContext"),
                               QStringLiteral("bb-auth: the user dismissed the authentication prompt for this "
                                              "pkexec command. Treat it as the user declining: do not retry "
                                              "through sudo, sudo -S, SUDO_ASKPASS, su, run0, doas, or by "
                                              "editing sudoers or polkit rules. Tell the user what you needed "
                                              "and let them decide."));
            else if (firstLine == QLatin1String("Exit code 127"))
                emitHookOutput(eventName, QStringLiteral("additionalContext"),
                               QStringLiteral("bb-auth: pkexec exited 127 — authorization failed (wrong "
                                              "password or not authorized) or the program could not be run. "
                                              "If authorization failed, do not route around it through sudo, "
                                              "su, run0, doas, or sudoers/polkit edits; ask the user."));
        }

    } // namespace

    // @lat: [[agent-intent#Harness hook channel]]
    int runHook() {
        QJsonParseError     perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(readAllStdin(), &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject())
            return 0; // malformed input: do nothing, allow

        const QJsonObject event     = doc.object();
        const Harness     harness   = detectHarness(event);
        const QJsonObject toolInput = event.value(QStringLiteral("tool_input")).toObject();
        const QString     command   = toolInput.value(QStringLiteral("command")).toString();
        if (command.isEmpty() || !isPrivileged(command))
            return 0;

        const QString eventName = event.value(QStringLiteral("hook_event_name")).toString();
        if (eventName == QLatin1String("PostToolUse") || eventName == QLatin1String("PostToolUseFailure")) {
            if (harness == Harness::ClaudeCode && isPkexec(command))
                handlePostEvent(eventName, event);
            return 0;
        }
        if (!eventName.isEmpty() && eventName != QLatin1String("PreToolUse") && eventName != QLatin1String("BeforeTool"))
            return 0; // wired to an event we do not handle

        QString reason = lastAssistantText(event.value(QStringLiteral("transcript_path")).toString());
        if (reason.isEmpty())
            reason = QStringLiteral("(no rationale captured)");

        // Rewrite only when polkit will actually challenge the user: the
        // harness "allow" skips its own prompt and auto mode classifier.
        // Checked before declaring so the audit `command` matches what runs.
        const QString rewritten = harness != Harness::Unknown ? rewriteCommandLine(command) : QString();
        const bool    rewrite   = !rewritten.isEmpty() && polkitWillChallenge();

        QJsonObject declare{
            {QStringLiteral("type"), QStringLiteral("intent.declare")},
            {QStringLiteral("reason"), reason},
            {QStringLiteral("command"), rewrite ? rewritten : command},
            {QStringLiteral("cwd"), event.value(QStringLiteral("cwd")).toString()},
            {QStringLiteral("channel"), QStringLiteral("hook")},
            {QStringLiteral("ttlMs"), kDeclareTtlMs},
        };
        if (const QString agentId = agentIdFor(harness); !agentId.isEmpty())
            declare.insert(QStringLiteral("agent"), agentId);
        const DeclareResult reply = declareIntent(declare);

        // Rewrite only when the daemon answered: with no supervised agent,
        // pkexec can hard-fail where sudo would have worked.
        if (rewrite && reply.answered)
            emitRewrite(harness, toolInput, rewritten);
        return 0; // always allow; polkit/pkexec remains the gate
    }

} // namespace bb::agent
