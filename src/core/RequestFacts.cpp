#include "RequestFacts.hpp"

#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

namespace bb {

    namespace {
        constexpr int kMaxArgs        = 256;
        constexpr int kMaxArgvChars   = 16 * 1024;
        constexpr int kMaxDetailKeys  = 16;
        constexpr int kMaxDetailKey   = 64;
        constexpr int kMaxDetailValue = 512;

        const QString kExecAction = QStringLiteral("org.freedesktop.policykit.exec");

        struct PkexecCall {
            bool        ok = false;
            bool        keepCwd = false;
            QString     program; // PROGRAM as the requester wrote it; empty opens a shell
            QStringList args;    // everything after PROGRAM
        };

        // pkexec [--keep-cwd] [--user|-u USER] [--disable-internal-agent] PROGRAM ARGS...
        // mirrors pkexec's own option loop (polkit 127 src/programs/pkexec.c):
        // the first unrecognised token is PROGRAM.
        PkexecCall parsePkexec(const QStringList &argv) {
            PkexecCall call;
            if (argv.isEmpty() || QFileInfo(argv[0]).fileName() != QLatin1String("pkexec"))
                return call;
            int n = 1;
            for (; n < argv.size(); ++n) {
                const QString &a = argv[n];
                if (a == QLatin1String("--keep-cwd"))
                    call.keepCwd = true;
                else if (a == QLatin1String("--disable-internal-agent") || a == QLatin1String("--help") || a == QLatin1String("--version"))
                    continue;
                else if (a == QLatin1String("--user") || a == QLatin1String("-u"))
                    ++n;
                else
                    break;
            }
            if (n > argv.size())
                return call;
            if (n < argv.size()) {
                call.program = argv[n];
                call.args    = argv.mid(n + 1);
            }
            call.ok = true;
            return call;
        }

        // pkexec reports command_line as the argv it will exec, joined by single
        // spaces: PROGRAM as written when absolute, else resolved through PATH
        // (never canonicalised, so multi-call binaries still see their name);
        // the target's shell alone when there is no PROGRAM. Returns that argv,
        // or nothing when command_line is not what this call would produce.
        std::optional<QStringList> execArgv(const PkexecCall &call, const QString &commandLine) {
            if (call.program.isEmpty()) {
                if (!commandLine.startsWith(QLatin1Char('/')) || commandLine.contains(QLatin1Char(' ')))
                    return std::nullopt;
                return QStringList{commandLine};
            }
            const QString tail = call.args.isEmpty() ? QString() : QLatin1Char(' ') + call.args.join(QLatin1Char(' '));
            if (!commandLine.endsWith(tail))
                return std::nullopt;
            const QString argv0 = commandLine.chopped(tail.size());
            if (call.program.startsWith(QLatin1Char('/'))) {
                if (argv0 != call.program)
                    return std::nullopt;
            } else if (!argv0.startsWith(QLatin1Char('/')) || !argv0.endsWith(QLatin1Char('/') + call.program)) {
                return std::nullopt;
            }
            return QStringList{argv0} + call.args;
        }
        // pkexec's message names the command through $(cmdline_short): the
        // command line when it is at most 80 bytes, else its first 38 bytes,
        // " ... " and its last 37 (polkit 127 pkexec.c).
        QString cmdlineShort(const QString &commandLine) {
            const QByteArray bytes = commandLine.toUtf8();
            if (bytes.size() <= 80)
                return commandLine;
            return QString::fromUtf8(bytes.left(38) + " ... " + bytes.right(37));
        }

        struct PkexecMessage {
            QString cmdlineShort;
            QString user;
        };

        // "Authentication is needed to run `CMD' as the super user", or
        // "... as user $(user.display)", which is "Gecos (login)" or "login".
        std::optional<PkexecMessage> parsePkexecMessage(const QString &message) {
            static const QRegularExpression re(QStringLiteral("^Authentication is needed to run `(.*)' as (?:(the super user)|user (.+))$"),
                                               QRegularExpression::DotMatchesEverythingOption);
            const auto match = re.match(message);
            if (!match.hasMatch())
                return std::nullopt;
            PkexecMessage parsed{match.captured(1), QStringLiteral("root")};
            if (match.captured(2).isEmpty()) {
                static const QRegularExpression login(QStringLiteral("\\(([^()]+)\\)$"));
                const QString display = match.captured(3);
                const auto    named   = login.match(display);
                parsed.user           = named.hasMatch() ? named.captured(1) : display;
            }
            return parsed;
        }

        // Without pkexec's details (polkit forwards only its own polkit.* keys
        // to agents), rebuild the command line from the caller's argv and
        // accept it only if it shortens to exactly what the message shows.
        // PROGRAM resolved through PATH is read off the message's first word.
        std::optional<QStringList> execArgvFromMessage(const PkexecCall &call, const QString &shown) {
            QString argv0;
            if (call.program.isEmpty() || call.program.startsWith(QLatin1Char('/'))) {
                argv0 = call.program.isEmpty() ? shown : call.program;
            } else {
                argv0 = shown.section(QLatin1Char(' '), 0, 0);
                if (!argv0.startsWith(QLatin1Char('/')) || !argv0.endsWith(QLatin1Char('/') + call.program))
                    return std::nullopt;
            }
            const QString full = call.args.isEmpty() ? argv0 : argv0 + QLatin1Char(' ') + call.args.join(QLatin1Char(' '));
            if (cmdlineShort(full) != shown)
                return std::nullopt;
            return execArgv(call, full);
        }

        // Root inside our user namespace reads as the overflow uid (the
        // service's sandbox maps only the session user), but capability sets
        // are reported unmapped: a caller holding CAP_SETUID is privileged.
        bool privileged(const ProcInfo &caller) {
            constexpr quint64 capSetuid = quint64(1) << 7;
            return caller.euid == 0 || (caller.capEff & capSetuid) != 0;
        }
    } // namespace

    // @lat: [[agent-intent#Verified request facts]]
    QJsonObject buildRequestFacts(const RequestFactsInput &input) {
        if (!input.caller || !privileged(*input.caller))
            return {};

        if (input.actionId != kExecAction) {
            QJsonObject details;
            for (auto it = input.details.cbegin(); it != input.details.cend(); ++it) {
                if (it.key().startsWith(QLatin1String("polkit.")) || it.key().size() > kMaxDetailKey)
                    continue;
                if (details.size() >= kMaxDetailKeys)
                    break;
                details[it.key()] = it.value().left(kMaxDetailValue);
            }
            return QJsonObject{{"kind", "action"}, {"actionId", input.actionId}, {"details", details}};
        }

        const PkexecCall call = parsePkexec(input.caller->argv);
        if (!call.ok)
            return {};
        // Anything else means the argv we read is not the one pkexec runs.
        std::optional<QStringList> verified;
        QString                    program = input.details.value(QStringLiteral("program"));
        QString                    user    = input.details.value(QStringLiteral("user"));
        const QString              commandLine = input.details.value(QStringLiteral("command_line"));
        if (!commandLine.isEmpty()) {
            if (!program.startsWith(QLatin1Char('/')))
                return {};
            verified = execArgv(call, commandLine);
        } else if (const auto message = parsePkexecMessage(input.message)) {
            verified = execArgvFromMessage(call, message->cmdlineShort);
            if (user.isEmpty())
                user = message->user;
        }
        if (!verified || verified->isEmpty())
            return {};
        const QStringList &argv = *verified;
        // pkexec's realpath()'d PROGRAM when it reported one; otherwise the
        // path it resolved, which may still be a symlink.
        if (program.isEmpty())
            program = argv.first();
        if (user.isEmpty())
            user = QStringLiteral("root");

        QJsonArray argvJson;
        int        chars     = 0;
        bool       truncated = false;
        for (const QString &a : argv) {
            if (argvJson.size() >= kMaxArgs || chars + a.size() > kMaxArgvChars) {
                truncated = true;
                break;
            }
            argvJson.append(a);
            chars += a.size();
        }

        QJsonObject   facts{{"kind", "exec"}, {"actionId", input.actionId}, {"program", program}, {"argv", argvJson}, {"user", user}};
        if (truncated)
            facts["truncated"] = true;
        // pkexec runs PROGRAM in the target user's home unless --keep-cwd.
        if (call.keepCwd) {
            if (!input.subjectCwd.isEmpty()) {
                facts["cwd"]     = input.subjectCwd;
                facts["cwdFrom"] = QStringLiteral("requester");
            }
        } else if (input.homeForUser) {
            const QString home = input.homeForUser(user);
            if (!home.isEmpty()) {
                facts["cwd"]     = home;
                facts["cwdFrom"] = QStringLiteral("target-home");
            }
        }
        return facts;
    }

} // namespace bb
