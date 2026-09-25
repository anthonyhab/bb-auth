// aisudo CLI (argv0 `aisudo`) and PATH-shim mode (argv0 `sudo`/`doas`/
// `pkexec`, symlinked under <libexec>/bb-auth-shims/).
//
// CLI: declare intent, apply the shared translation, exec. A bare
// `aisudo CMD` means `sudo CMD`. Fails open: a daemon that does not answer
// leaves the command unchanged.
//
// Shim: active only under a recognized agent ancestor — declares a generic
// intent (channel "shim") and translates; for humans it execs the real
// binary verbatim. Passthrough never declares, so it cannot clobber a real
// reason declared moments earlier (the daemon correlates latest-wins).

#include "Common.hpp"
#include "Translate.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QSet>

#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

namespace bb::agent {

    namespace {

        const QString    kShimReason = QStringLiteral("Agent ran a privileged command without declaring a reason");
        const QByteArray kUsage      = "usage: aisudo [-r WHY] [--json JSON|-] [--dry-run] [--] [sudo|doas|pkexec] CMD...\n";

        const char kHelp[] = R"(usage: aisudo [-r WHY] [--agent NAME] [--json JSON|-] [--dry-run] [--] [LAUNCHER] CMD...
       aisudo --print-shim-dir

Declare agent intent to bb-auth, then exec the command — elevation routes
through the supervised polkit prompt.

options:
  -r, --reason TEXT   why (shown at the auth prompt); defaults to the command
  --agent NAME        agent identifier (display/audit only; auto-detected)
  --json JSON|-       structured input object, or - to read it from stdin
  --dry-run           print the resolved plan and exit
  --print-shim-dir    print the installed PATH-shim directory and exit
  -h, --help          show this help

the name is the verb: `aisudo CMD` == `aisudo sudo CMD` -> pkexec. An explicit
sudo/doas/pkexec launcher inside the command is accepted and absorbed.

sudo/doas subset that translates to pkexec: -n/--non-interactive (dropped),
-u/--user USER, --. Other options, VAR=val, and shell metacharacters run
unchanged via their own launcher. `sudo -n` with no command is a probe and
never translates.

JSON mode:  --json '{"argv": ["sudo","make","install"], "reason": "why"}'
            --json -  reads the object from stdin; 'command' (string) is an
            accepted alternative to 'argv'. Optional keys: reason, agent, cwd.
--dry-run   prints {"declare": ..., "exec": ..., "fallback_exec": ...} and
            exits without contacting the daemon or executing anything.
)";

        [[noreturn]] void die(const QString &msg) {
            writeErr("aisudo: " + msg.toUtf8() + "\naisudo: " + kUsage);
            std::exit(2);
        }

        void eprint(const QString &msg) {
            writeErr("aisudo: " + msg.toUtf8() + '\n');
        }

        QString currentDir() {
            const QString cwd = QDir::currentPath();
            return QFileInfo::exists(cwd) ? cwd : QString();
        }

        // First PATH entry that is an executable file and not this binary
        // (compared by inode, so shim symlinks resolve to "self").
        QString realBinary(const QString &name) {
            struct stat self{};
            const bool  haveSelf = ::stat("/proc/self/exe", &self) == 0;
            for (const QString &dir : qEnvironmentVariable("PATH").split(QLatin1Char(':'))) {
                const QString    cand = (dir.isEmpty() ? QStringLiteral(".") : dir) + QLatin1Char('/') + name;
                const QByteArray c    = cand.toLocal8Bit();
                struct stat      st{};
                if (::stat(c.constData(), &st) != 0 || !S_ISREG(st.st_mode) || ::access(c.constData(), X_OK) != 0)
                    continue;
                if (haveSelf && st.st_dev == self.st_dev && st.st_ino == self.st_ino)
                    continue;
                return cand;
            }
            return {};
        }

        [[noreturn]] void execReal(const QString &name, const QStringList &args) {
            const QString real = realBinary(name);
            if (!real.isEmpty())
                execPath(real, QStringList{name} + args);
            writeErr("bb-auth: cannot find the real " + name.toUtf8() + " on PATH\n");
            std::_Exit(127);
        }

        QByteArray reportLine(const DeclareResult &r) {
            if (!r.answered)
                return "bb-auth: intent declaration unavailable; command continues without attribution\n";
            return r.bound ? QByteArray("bb-auth: intent declared\n")
                           : QByteArray("bb-auth: intent not bound to a recognized agent; prompt will use human layout\n");
        }

        QJsonObject declarePayload(const QString &reason, const QString &agent, const QString &command,
                                   const QString &cwd, const QString &channel) {
            return QJsonObject{
                {QStringLiteral("type"), QStringLiteral("intent.declare")},
                {QStringLiteral("reason"), reason.left(kMaxReason)},
                {QStringLiteral("agent"), agent},
                {QStringLiteral("command"), command},
                {QStringLiteral("cwd"), cwd},
                {QStringLiteral("channel"), channel},
                {QStringLiteral("ttlMs"), kDeclareTtlMs},
            };
        }

        struct JsonInput {
            QStringList argv;
            QString     reason, agent, cwd;
        };

        JsonInput parseJsonInput(QString raw) {
            if (raw == QLatin1String("-")) {
                QByteArray in;
                char       buf[4096];
                ssize_t    n;
                while ((n = ::read(STDIN_FILENO, buf, sizeof(buf))) > 0)
                    in.append(buf, static_cast<int>(n));
                raw = QString::fromUtf8(in);
            }
            QJsonParseError     err{};
            const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8(), &err);
            if (err.error != QJsonParseError::NoError)
                die(QStringLiteral("invalid --json: %1; expected e.g. '{\"argv\": [\"sudo\", \"make\", \"install\"], \"reason\": \"why\"}'")
                        .arg(err.errorString()));
            if (!doc.isObject())
                die(QStringLiteral("invalid --json: expected an object like {\"argv\": [\"sudo\", \"make\", \"install\"]}"));
            const QJsonObject   obj = doc.object();
            static const QStringList allowed{QStringLiteral("argv"), QStringLiteral("command"), QStringLiteral("reason"),
                                             QStringLiteral("agent"), QStringLiteral("cwd")};
            QStringList bad;
            for (const QString &k : obj.keys())
                if (!allowed.contains(k))
                    bad << k;
            if (!bad.isEmpty())
                die(QStringLiteral("unknown --json keys [%1]; accepted: [%2]").arg(bad.join(QStringLiteral(", ")), allowed.join(QStringLiteral(", "))));
            JsonInput in;
            if (obj.contains(QStringLiteral("argv"))) {
                const QJsonValue v = obj.value(QStringLiteral("argv"));
                if (!v.isArray())
                    die(QStringLiteral("--json 'argv' must be an array of strings"));
                for (const QJsonValue &a : v.toArray()) {
                    if (!a.isString())
                        die(QStringLiteral("--json 'argv' must be an array of strings"));
                    in.argv << a.toString();
                }
            } else if (obj.contains(QStringLiteral("command"))) {
                if (!obj.value(QStringLiteral("command")).isString())
                    die(QStringLiteral("--json 'command' must be a string"));
                in.argv = QProcess::splitCommand(obj.value(QStringLiteral("command")).toString());
            }
            for (const char *key : {"reason", "agent", "cwd"}) {
                const QJsonValue v = obj.value(QLatin1String(key));
                if (!v.isUndefined() && !v.isNull() && !v.isString())
                    die(QStringLiteral("--json '%1' must be a string").arg(QLatin1String(key)));
            }
            in.reason = obj.value(QStringLiteral("reason")).toString();
            in.agent  = obj.value(QStringLiteral("agent")).toString();
            in.cwd    = obj.value(QStringLiteral("cwd")).toString();
            return in;
        }

    } // namespace

    // @lat: [[agent-intent#PATH shim channel]]
    int runShim(const QString &tool, const QStringList &args) {
        const QString     agent = detectAgentAncestor();
        const Translation t     = agent.isEmpty() ? Translation{} : translateArgv(tool, args);
        if (!t.ok())
            execReal(tool, args);
        const DeclareResult r = declareIntent(declarePayload(kShimReason, agent, (QStringList{tool} + args).join(QLatin1Char(' ')),
                                                             currentDir(), QStringLiteral("shim")));
        // Daemon answered (bound or not): the supervised path is live — rewrite.
        // No daemon: keep the command on its original auth path.
        if (r.answered)
            execReal(t.argv.first(), t.argv.mid(1));
        execReal(tool, args);
    }

    // @lat: [[agent-intent#Agent CLI channel]]
    int runAisudo(const QStringList &argv) {
        QString     reason, agentOverride, jsonInput;
        bool        haveJson = false, dryRun = false;
        QStringList cmd;

        // Leading aisudo options only; the first unknown token — including a
        // bare launcher flag like `-n` — starts the command.
        int i = 0;
        for (; i < argv.size(); ++i) {
            const QString &a       = argv[i];
            const auto     needArg = [&](const QString &opt) {
                if (i + 1 >= argv.size())
                    die(opt + QStringLiteral(" needs a value"));
                return argv[++i];
            };
            if (a == QLatin1String("--")) {
                ++i;
                break;
            }
            if (a == QLatin1String("-r") || a == QLatin1String("--reason"))
                reason = needArg(a);
            else if (a.startsWith(QLatin1String("--reason=")))
                reason = a.mid(9);
            else if (a.startsWith(QLatin1String("-r")) && a.size() > 2)
                reason = a.mid(2);
            else if (a == QLatin1String("--agent"))
                agentOverride = needArg(a);
            else if (a.startsWith(QLatin1String("--agent=")))
                agentOverride = a.mid(8);
            else if (a == QLatin1String("--json")) {
                jsonInput = needArg(a);
                haveJson  = true;
            } else if (a.startsWith(QLatin1String("--json="))) {
                jsonInput = a.mid(7);
                haveJson  = true;
            } else if (a == QLatin1String("--dry-run"))
                dryRun = true;
            else if (a == QLatin1String("--print-shim-dir")) {
                writeOut(shimDir().toUtf8() + '\n');
                return 0;
            } else if (a == QLatin1String("-h") || a == QLatin1String("--help")) {
                writeOut(kHelp);
                return 0;
            } else if (a == QLatin1String("--version")) {
                writeOut("aisudo " BB_AUTH_VERSION "\n");
                return 0;
            } else
                break;
        }
        cmd = argv.mid(i);

        QString cwd;
        if (haveJson) {
            if (!cmd.isEmpty())
                die(QStringLiteral("--json cannot be combined with a positional command"));
            const JsonInput in = parseJsonInput(jsonInput);
            cmd                = in.argv;
            if (reason.isEmpty())
                reason = in.reason;
            if (agentOverride.isEmpty())
                agentOverride = in.agent;
            cwd = in.cwd;
        }
        if (cmd.isEmpty() && reason.isEmpty())
            die(QStringLiteral("--reason is required when no command is given"));

        // The name is the verb: bare commands normalize to sudo.
        QString     launcher;
        QStringList launcherArgs, fallback;
        if (!cmd.isEmpty()) {
            const QString base = QFileInfo(cmd.first()).fileName();
            if (base == QLatin1String("sudo") || base == QLatin1String("doas") || base == QLatin1String("pkexec")) {
                launcher     = base;
                launcherArgs = cmd.mid(1);
                fallback     = cmd;
            } else {
                launcher     = QStringLiteral("sudo");
                launcherArgs = cmd;
                fallback     = QStringList{QStringLiteral("sudo")} + cmd;
            }
        }
        const QString declaredCmd = fallback.join(QLatin1Char(' '));
        if (reason.isEmpty())
            reason = declaredCmd;
        QString agent = agentOverride;
        if (agent.isEmpty())
            agent = detectAgentAncestor();
        if (agent.isEmpty())
            agent = QStringLiteral("agent");
        if (cwd.isEmpty())
            cwd = currentDir();

        const QJsonObject payload = declarePayload(reason, agent, declaredCmd, cwd, QStringLiteral("cli"));

        if (dryRun) {
            const Translation t = cmd.isEmpty() ? Translation{} : translateArgv(launcher, launcherArgs);
            QJsonObject       plan{{QStringLiteral("declare"), payload},
                             {QStringLiteral("exec"), QJsonArray::fromStringList(t.ok() ? t.argv : fallback)},
                             {QStringLiteral("note"), t.note}};
            if (t.ok())
                plan.insert(QStringLiteral("fallback_exec"), QJsonArray::fromStringList(fallback));
            writeOut(QJsonDocument(plan).toJson(QJsonDocument::Compact) + '\n');
            return 0;
        }

        const DeclareResult r = declareIntent(payload);
        writeErr(reportLine(r));

        QStringList execArgv = fallback;
        if (!cmd.isEmpty() && r.answered) {
            const Translation t = translateArgv(launcher, launcherArgs);
            if (t.ok()) {
                execArgv = t.argv;
                eprint(QStringLiteral("%1 → %2%3").arg(declaredCmd, execArgv.join(QLatin1Char(' ')),
                                                           t.note.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(t.note)));
            } else if (!t.note.isEmpty()) {
                eprint(QStringLiteral("%1; running %2 unchanged").arg(t.note, declaredCmd));
            }
        }
        if (execArgv.isEmpty())
            return 0;
        execSearch(execArgv);
        eprint(QStringLiteral("cannot execute %1").arg(execArgv.first()));
        return 127;
    }

} // namespace bb::agent
