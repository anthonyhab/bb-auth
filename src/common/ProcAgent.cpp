#include "ProcAgent.hpp"

#include <QByteArray>
#include <QDebug>
#include <QLatin1String>
#include <QStringList>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

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

namespace bb {

std::optional<ProcInfo> readProc(qint64 pid) {
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

// @lat: [[agent-intent#Intent declaration and correlation]]
AgentMatch detectAgent(const ProcInfo& proc) {
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
        // pi runs as `node …/pi-coding-agent/…/cli.js`; the package dir is the
        // signature. Bare `pi` is deliberately absent — it collides with the
        // unrelated Debian `pi` utility of the same name.
        {"pi-coding-agent", "pi", "pi", "pi"},
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

} // namespace bb
