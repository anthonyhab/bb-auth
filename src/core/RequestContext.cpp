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
