#include "RequestContext.hpp"
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QDir>
#include <QDirIterator>
#include <QStandardPaths>
#include <QSettings>
#include <QProcess>
#include <QDebug>
#include <iostream>

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>

QJsonObject ProcInfo::toJson() const {
    QJsonObject obj;
    if (pid > 0)
        obj["pid"] = pid;
    if (ppid > 0)
        obj["ppid"] = ppid;
    if (uid >= 0)
        obj["uid"] = uid;
    if (euid >= 0)
        obj["euid"] = euid;
    if (!name.isEmpty())
        obj["name"] = name;
    if (!exe.isEmpty())
        obj["exe"] = exe;
    if (!cmdline.isEmpty())
        obj["cmdline"] = cmdline;
    return obj;
}

QJsonObject ActorInfo::toJson() const {
    QJsonObject obj;
    obj["proc"] = proc.toJson();
    if (desktop.isValid()) {
        obj["desktopId"] = desktop.desktopId;
    }
    obj["displayName"]    = displayName;
    obj["iconName"]       = iconName;
    obj["fallbackLetter"] = fallbackLetter;
    obj["fallbackKey"]    = fallbackKey;
    obj["confidence"]     = confidence;
    if (isAgent) {
        obj["isAgent"]   = true;
        obj["agentKind"] = agentKind;
    }
    return obj;
}

std::optional<qint64> RequestContextHelper::extractSubjectPid(const PolkitQt1::Details& details) {
    bool   ok  = false;
    qint64 pid = details.lookup("polkit.subject-pid").toLongLong(&ok);
    if (ok && pid > 0)
        return pid;

    pid = details.lookup("polkit.caller-pid").toLongLong(&ok);
    if (ok && pid > 0)
        return pid;

    return std::nullopt;
}

std::optional<qint64> RequestContextHelper::extractCallerPid(const PolkitQt1::Details& details) {
    bool   ok  = false;
    qint64 pid = details.lookup("polkit.caller-pid").toLongLong(&ok);
    if (ok && pid > 0)
        return pid;
    return std::nullopt;
}

namespace {

    // Read a small /proc file relative to a pinned dir fd. Returns empty on any error
    // (including the pid having been recycled out from under us).
    QByteArray readProcEntry(int dirfd, const char* name) {
        const int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return {};
        }
        QByteArray out;
        char       buf[4096];
        for (;;) {
            const ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0) {
                out.append(buf, static_cast<int>(n));
                if (out.size() > 256 * 1024) // sanity bound; /proc entries are tiny
                    break;
            } else if (n == 0) {
                break;
            } else if (errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
        close(fd);
        return out;
    }

    // starttime is /proc/<pid>/stat field 22. comm (field 2) is wrapped in parens and may
    // itself contain spaces/parens, so anchor parsing on the LAST ')' and count from there.
    qint64 parseStartTime(const QByteArray& stat) {
        const int rparen = stat.lastIndexOf(')');
        if (rparen < 0)
            return 0;
        const QList<QByteArray> rest = stat.mid(rparen + 1).simplified().split(' ');
        // After ')', token[0] is field 3 (state); field 22 (starttime) is token[19].
        if (rest.size() <= 19)
            return 0;
        return rest[19].toLongLong();
    }

} // namespace

std::optional<ProcInfo> RequestContextHelper::readProc(qint64 pid) {
    ProcInfo info;
    info.pid = pid;

    // Pin /proc/<pid> via a directory fd and read every field relative to it. This
    // guarantees all fields describe the SAME process: if the pid is recycled while we
    // read, the openat()s below fail rather than silently mixing another process's data
    // (the PID-reuse TOCTOU). Note: the window between polkit's measurement and this
    // open cannot be closed here without an authoritative start-time from polkit.
    const QByteArray procPath = QByteArrayLiteral("/proc/") + QByteArray::number(pid);
    const int        dirfd    = open(procPath.constData(), O_DIRECTORY | O_RDONLY | O_CLOEXEC);
    if (dirfd < 0) {
        qDebug() << "readProc: cannot open" << procPath << "errno" << errno;
        return std::nullopt;
    }

    // 1. status — name, ppid, uid/euid (world-readable)
    const QByteArray status = readProcEntry(dirfd, "status");
    if (status.isEmpty()) {
        qDebug() << "readProc: /proc/" << pid << "/status unreadable (pid gone?)";
        close(dirfd);
        return std::nullopt;
    }
    for (const auto& line : QString::fromUtf8(status).split('\n')) {
        if (line.startsWith("Name:")) {
            info.name = line.section(':', 1).trimmed();
        } else if (line.startsWith("PPid:")) {
            info.ppid = line.section(':', 1).trimmed().toLongLong();
        } else if (line.startsWith("Uid:")) {
            const QStringList parts = line.section(':', 1).simplified().split(' ');
            if (parts.size() >= 1)
                info.uid = parts[0].toLongLong();
            if (parts.size() >= 2)
                info.euid = parts[1].toLongLong();
        }
    }

    // 2. exe symlink (may fail for setuid/root targets — non-fatal)
    {
        char    buf[4096];
        ssize_t n = readlinkat(dirfd, "exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n]   = '\0';
            info.exe = QString::fromUtf8(buf, static_cast<int>(n));
        }
    }

    // 3. cmdline (NUL-separated argv)
    const QByteArray cmdline = readProcEntry(dirfd, "cmdline");
    if (!cmdline.isEmpty()) {
        QStringList cleanArgs;
        for (const auto& a : cmdline.split('\0'))
            if (!a.isEmpty())
                cleanArgs << QString::fromUtf8(a);
        info.cmdline = cleanArgs.join(" ");
    }

    // 4. starttime — pins process generation; lets callers detect pid reuse across reads
    info.startTime = parseStartTime(readProcEntry(dirfd, "stat"));

    close(dirfd);
    return info;
}

static QList<DesktopInfo> g_desktopIndex;
static bool               g_indexDone = false;

void                      RequestContextHelper::ensureDesktopIndex() {
    if (g_indexDone)
        return;
    g_indexDone = true;

    QStringList paths = QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
    for (const auto& path : paths) {
        QDirIterator it(path, QStringList() << "*.desktop", QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString   file = it.next();
            QSettings settings(file, QSettings::IniFormat);
            settings.beginGroup("Desktop Entry");

            if (settings.value("NoDisplay", false).toBool())
                continue;

            DesktopInfo d;
            d.desktopId = QFileInfo(file).fileName();
            d.name      = settings.value("Name").toString();
            d.iconName  = settings.value("Icon").toString();
            d.exec      = settings.value("Exec").toString().split(' ').first().remove('"');
            d.tryExec   = settings.value("TryExec").toString();

            // We'll store the Exec string too for matching if needed,
            // but for now let's just use the index.
            // Matching will be done by filename vs exe basename mostly.

            if (!d.name.isEmpty()) {
                g_desktopIndex << d;
            }
        }
    }
}

DesktopInfo RequestContextHelper::findDesktopForExe(const QString& exePath) {
    ensureDesktopIndex();
    if (exePath.isEmpty())
        return {};

    QString base = QFileInfo(exePath).fileName();

    // 1. Exact match <base>.desktop
    for (const auto& d : g_desktopIndex) {
        if (d.desktopId == base + ".desktop")
            return d;
    }

    // 2. Case-insensitive match
    for (const auto& d : g_desktopIndex) {
        if (d.desktopId.compare(base + ".desktop", Qt::CaseInsensitive) == 0)
            return d;
    }

    // 3. Match by Exec basename
    for (const auto& d : g_desktopIndex) {
        if (!d.exec.isEmpty() && QFileInfo(d.exec).fileName() == base)
            return d;
    }

    // 4. Match by TryExec basename
    for (const auto& d : g_desktopIndex) {
        if (!d.tryExec.isEmpty() && QFileInfo(d.tryExec).fileName() == base)
            return d;
    }

    // 5. Match by Name (case-insensitive)
    for (const auto& d : g_desktopIndex) {
        if (d.name.compare(base, Qt::CaseInsensitive) == 0)
            return d;
    }

    return {};
}

AgentMatch RequestContextHelper::detectAgent(const ProcInfo& proc) {
    // Token-aware matching: a process is a known agent only when an argv token's
    // basename (or the exe/comm name) *is* a known alias — never when a needle
    // merely appears anywhere in the cmdline. Raw substring matching let
    // `vim devin-notes.md` impersonate an agent ancestor; word-boundary aliases
    // keep `agy` safe from "strategy" while still matching real invocations
    // (`/usr/bin/claude`, `npx @anthropic-ai/claude-code`, `node …/codex.js`).
    struct Alias {
        const char* name;
        const char* kind;
        const char* displayName;
        const char* iconName;
    };
    static const Alias kAliases[] = {
        {"claude", "claude-code", "Claude Code", "claude-code"},
        {"claude-code", "claude-code", "Claude Code", "claude-code"},
        {"codex", "codex", "Codex CLI", "codex"},
        {"codex-cli", "codex", "Codex CLI", "codex"},
        {"gemini", "gemini-cli", "Gemini CLI", "gemini-cli"},
        {"gemini-cli", "gemini-cli", "Gemini CLI", "gemini-cli"},
        {"agy", "gemini-cli", "Gemini CLI", "gemini-cli"},
        {"aider", "aider", "Aider", "aider"},
        {"aider-chat", "aider", "Aider", "aider"},
        {"cursor-agent", "cursor-agent", "Cursor Agent", "cursor-agent"},
        {"opencode", "opencode", "OpenCode", "opencode"},
        {"copilot", "copilot-cli", "Copilot CLI", "copilot-cli"},
        {"copilot-cli", "copilot-cli", "Copilot CLI", "copilot-cli"},
        {"gh-copilot", "copilot-cli", "Copilot CLI", "copilot-cli"},
        {"devin", "devin", "Devin", "devin"},
        {"devin-cli", "devin", "Devin", "devin"},
    };
    static const char* kScriptExts[] = {".js", ".mjs", ".cjs", ".py", ".exe", ".cmd"};

    const auto aliasExact = [](const QString& base) -> const Alias* {
        for (const auto& a : kAliases)
            if (base == QLatin1String(a.name))
                return &a;
        return nullptr;
    };
    const auto aliasOfBase = [&](const QString& base) -> const Alias* {
        if (const Alias* a = aliasExact(base))
            return a;
        // Script-entry forms: `node …/codex.js`, `claude.exe`, `aider.py`.
        for (const auto& a : kAliases) {
            const QString alias = QLatin1String(a.name);
            for (const char* ext : kScriptExts)
                if (base == alias + QLatin1String(ext))
                    return &a;
        }
        return nullptr;
    };
    // A token can be a path — check every '/'-segment for an exact alias (real
    // installs live under dirs like /opt/claude-code or ~/.cursor-agent), then
    // the basename for script-entry forms. Segment equality still rejects
    // `devin-notes.md`-style collisions.
    const auto aliasOfToken = [&](const QString& token) -> const Alias* {
        const QStringList segs = token.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (const QString& seg : segs)
            if (const Alias* a = aliasExact(seg.toLower()))
                return a;
        return segs.isEmpty() ? nullptr : aliasOfBase(segs.last().toLower());
    };

    if (const Alias* a = aliasExact(proc.name.toLower()))
        return AgentMatch{QString::fromLatin1(a->kind), QString::fromLatin1(a->displayName), QString::fromLatin1(a->iconName)};

    if (const Alias* a = aliasOfToken(proc.exe))
        return AgentMatch{QString::fromLatin1(a->kind), QString::fromLatin1(a->displayName), QString::fromLatin1(a->iconName)};

    const QStringList tokens = proc.cmdline.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
        if (const Alias* a = aliasOfToken(token))
            return AgentMatch{QString::fromLatin1(a->kind), QString::fromLatin1(a->displayName), QString::fromLatin1(a->iconName)};
    }
    return {};
}

ActorInfo RequestContextHelper::resolveRequestorFromSubject(const ProcInfo& subject, qint64 agentUid) {
    return resolveRequestorFromSubject(subject, agentUid, [](qint64 pid) { return readProc(pid); });
}

ActorInfo RequestContextHelper::resolveRequestorFromSubject(const ProcInfo& subject, qint64 agentUid, std::function<std::optional<ProcInfo>(qint64)> procReader) {
    ActorInfo actor;
    actor.proc = subject;

    qDebug() << "Resolving requestor from PID" << subject.pid << "(uid=" << subject.uid << ", exe=" << subject.exe << ")";

    qint64 currPid = subject.pid;
    int    hops    = 0;

    while (currPid > 1 && hops < 16) {
        auto info = procReader(currPid);
        if (!info) {
            qDebug() << "Requestor resolution: failed to read /proc for pid" << currPid;
            break;
        }

        qDebug() << "Requestor resolution: pid" << info->pid << "(name=" << info->name << ", ppid=" << info->ppid << ", uid=" << info->uid << ", exe=" << info->exe << ")";

        QString exeName;
        if (!info->exe.isEmpty()) {
            exeName = QFileInfo(info->exe).fileName();
        } else if (info->euid == 0) {
            exeName = info->name;
        }

        bool isBridge = (exeName == "pkexec" || exeName == "sudo" || exeName == "doas");

        // Skip processes not owned by the user (agent) unless it's a known bridge like pkexec
        if (info->uid != agentUid && agentUid != 0 && !isBridge) {
            qDebug() << "Requestor resolution: stopping at pid" << info->pid << "(uid mismatch)";
            break;
        }

        // If this is a user process (not root/bridge), keep it as a fallback candidate
        if (!isBridge && info->uid == agentUid) {
            actor.proc = *info;
        }

        // Agent attribution wins over a desktop match: for `pkexec -> node(claude) ->
        // terminal` we want "Claude Code", not the terminal emulator. Check the agent
        // registry before .desktop matching and stop at the first recognized agent.
        if (const AgentMatch agent = detectAgent(*info); agent.isValid()) {
            actor.proc        = *info;
            actor.isAgent     = true;
            actor.agentKind   = agent.kind;
            actor.displayName = agent.displayName;
            actor.iconName    = agent.iconName;
            actor.confidence  = "agent";
            qDebug() << "Requestor resolution: matched agent" << agent.kind << "at pid" << info->pid;
            break;
        }

        DesktopInfo d;
        if (!info->exe.isEmpty()) {
            d = findDesktopForExe(info->exe);
        }

        if (!d.isValid() && !info->name.isEmpty()) {
            d = findDesktopForExe(info->name);
        }

        if (d.isValid()) {
            actor.proc       = *info;
            actor.desktop    = d;
            actor.confidence = "desktop";
            qDebug() << "Requestor resolution: matched desktop entry" << d.desktopId << "(icon=" << d.iconName << ", name=" << d.name << ")";
            break;
        }

        if (info->ppid <= 1 || info->ppid == currPid) {
            qDebug() << "Requestor resolution: stopping at pid" << info->pid << "(ppid=" << info->ppid << ")";
            break;
        }
        currPid = info->ppid;
        hops++;
    }

    if (!actor.isAgent && !actor.desktop.isValid()) {
        actor.confidence = actor.proc.exe.isEmpty() ? (actor.proc.name.isEmpty() ? "unknown" : "name-only") : "exe-only";
    }

    // Fill display names (agent attribution already set displayName/iconName above)
    if (actor.isAgent) {
        // keep agent-resolved displayName/iconName
    } else if (actor.desktop.isValid()) {
        actor.displayName = actor.desktop.name;
        actor.iconName    = actor.desktop.iconName;
    } else if (!actor.proc.exe.isEmpty()) {
        actor.displayName = QFileInfo(actor.proc.exe).fileName();
        if (actor.iconName.isEmpty()) {
            actor.iconName = QFileInfo(actor.proc.exe).baseName().toLower();
        }
    } else if (!actor.proc.name.isEmpty()) {
        actor.displayName = actor.proc.name;
        if (actor.iconName.isEmpty()) {
            actor.iconName = actor.proc.name.toLower();
        }
    } else {
        actor.displayName = "Unknown";
    }

    if (!actor.displayName.isEmpty()) {
        actor.fallbackLetter = actor.displayName.at(0).toUpper();
    }

    actor.fallbackKey = actor.desktop.isValid() ? actor.desktop.desktopId : actor.displayName.toLower();

    return actor;
}

QString RequestContextHelper::normalizePrompt(QString s) {
    s = s.trimmed();
    if (s.endsWith(':'))
        s.chop(1);
    else if (s.endsWith(u'：'))
        s.chop(1);
    return s.trimmed();
}

QJsonObject RequestContextHelper::classifyRequest(const QString& source, const QString& title, const QString& description) {
    QJsonObject hint;
    QString     kind     = "unknown";
    QString     icon     = "";
    bool        colorize = false;

    if (source == "polkit") {
        kind     = "polkit";
        icon     = "security-high";
        colorize = true; // Polkit requests usually look good colorized
    } else if (source == "keyring") {
        if (title.contains("gpg", Qt::CaseInsensitive) || description.contains("OpenPGP", Qt::CaseInsensitive)) {
            kind     = "gpg";
            icon     = "gnupg";
            colorize = true;
        } else if (title.contains("ssh", Qt::CaseInsensitive) || description.contains("ssh", Qt::CaseInsensitive)) {
            kind     = "ssh";
            icon     = "ssh-key";
            colorize = true;
        } else {
            kind     = "keyring";
            colorize = true;
        }
    }

    hint["kind"]     = kind;
    hint["colorize"] = colorize;
    if (!icon.isEmpty())
        hint["iconName"] = icon;

    return hint;
}
