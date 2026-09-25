#include "Translate.hpp"

#include <QRegularExpression>

namespace bb::agent {

    namespace {
        const QRegularExpression &metaChars() {
            static const QRegularExpression re(QStringLiteral("[|&;<>`$()\\n]"));
            return re;
        }
    } // namespace

    // @lat: [[agent-intent#Escalation translation]]
    Translation translateArgv(const QString &launcher, const QStringList &args) {
        if (launcher != QLatin1String("sudo") && launcher != QLatin1String("doas")) {
            return {{}, launcher == QLatin1String("pkexec") ? QStringLiteral("already pkexec")
                                                            : launcher + QStringLiteral(" is not a translatable launcher")};
        }
        int         i = 0;
        QString     user;
        bool        haveUser = false;
        QStringList notes;
        while (i < args.size()) {
            const QString &a = args[i];
            if (a == QLatin1String("--")) {
                ++i;
                break;
            }
            if (a == QLatin1String("-n") || a == QLatin1String("--non-interactive")) {
                notes << QStringLiteral("dropped -n: GUI prompt replaces stdin auth");
                ++i;
                continue;
            }
            if (a == QLatin1String("-u") || a == QLatin1String("--user")) {
                if (i + 1 >= args.size())
                    return {{}, launcher + QLatin1Char(' ') + a + QStringLiteral(" needs a user argument")};
                user     = args[i + 1];
                haveUser = true;
                i += 2;
                continue;
            }
            if (a.startsWith(QLatin1String("--user="))) {
                user     = a.mid(7);
                haveUser = true;
                ++i;
                continue;
            }
            if (a.startsWith(QLatin1String("-u")) && a.size() > 2) {
                user     = a.mid(2);
                haveUser = true;
                ++i;
                continue;
            }
            if (a.startsWith(QLatin1Char('-')))
                return {{}, launcher + QLatin1Char(' ') + a + QStringLiteral(" not translatable to pkexec")};
            if (a.contains(QLatin1Char('=')))
                return {{}, QStringLiteral("env assignment not translatable to pkexec")};
            break;
        }
        const QStringList rest = args.mid(i);
        if (rest.isEmpty())
            return {{}, launcher + QStringLiteral(" without a command is a probe; nothing to elevate")};
        for (const QString &a : rest)
            if (metaChars().match(a).hasMatch())
                return {{}, QStringLiteral("shell metacharacters in command; declining")};
        if (haveUser && metaChars().match(user).hasMatch())
            return {{}, QStringLiteral("shell metacharacters in command; declining")};
        QStringList argv{QStringLiteral("pkexec")};
        if (haveUser)
            argv << QStringLiteral("--user") << user;
        argv << rest;
        return {argv, notes.join(QStringLiteral("; "))};
    }

    // @lat: [[agent-intent#sudo to pkexec rewrite]]
    QString rewriteCommandLine(const QString &command) {
        static const QRegularExpression leadTok(QStringLiteral("^(\\S+)\\s*"));
        const QString                   trimmed = command.trimmed();
        if (!trimmed.startsWith(QLatin1String("sudo ")) && !trimmed.startsWith(QLatin1String("doas ")))
            return {};
        if (metaChars().match(trimmed).hasMatch())
            return {};
        QString rest = trimmed.mid(trimmed.indexOf(QLatin1Char(' '))).trimmed();
        QString user;
        for (;;) {
            const auto m = leadTok.match(rest);
            if (!m.hasMatch())
                return {}; // probe / options with no command
            const QString tok = m.captured(1);
            if (tok == QLatin1String("--")) {
                rest = rest.mid(m.capturedLength());
                break;
            }
            if (tok == QLatin1String("-n") || tok == QLatin1String("--non-interactive")) {
                rest = rest.mid(m.capturedLength());
                continue;
            }
            if (tok == QLatin1String("-u") || tok == QLatin1String("--user")) {
                rest         = rest.mid(m.capturedLength());
                const auto v = leadTok.match(rest);
                if (!v.hasMatch())
                    return {}; // missing user argument
                user = v.captured(1);
                rest = rest.mid(v.capturedLength());
                continue;
            }
            if (tok.startsWith(QLatin1String("--user="))) {
                user = tok.mid(7);
                rest = rest.mid(m.capturedLength());
                continue;
            }
            if (tok.startsWith(QLatin1String("-u")) && tok.size() > 2) {
                user = tok.mid(2);
                rest = rest.mid(m.capturedLength());
                continue;
            }
            if (tok.startsWith(QLatin1Char('-')) || tok.contains(QLatin1Char('=')))
                return {}; // unsupported option / env assignment
            break;         // first non-option token: command begins
        }
        if (rest.isEmpty())
            return {};
        return QStringLiteral("pkexec") + (user.isEmpty() ? QString() : QStringLiteral(" --user ") + user) +
            QLatin1Char(' ') + rest;
    }

} // namespace bb::agent
