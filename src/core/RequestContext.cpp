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

QJsonObject ProcInfo::toJson() const {
    QJsonObject obj;
    if (pid > 0)
        obj["pid"] = pid;
    if (ppid > 0)
        obj["ppid"] = ppid;
    if (uid >= 0)
        obj["uid"] = uid;
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
    obj["displayName"] = displayName;
    if (!iconName.isEmpty())
        obj["iconName"] = iconName;
    obj["fallbackLetter"] = fallbackLetter;
    obj["fallbackKey"]    = fallbackKey;
    obj["confidence"]     = confidence;
    return obj;
}

std::optional<qint64> RequestContextHelper::extractSubjectPid(const PolkitQt1::Details& details) {
    QString pidStr = details.lookup("polkit.subject-pid");
    if (pidStr.isEmpty())
        pidStr = details.lookup("polkit.caller-pid");

    bool   ok  = false;
    qint64 pid = pidStr.toLongLong(&ok);
    if (ok && pid > 0)
        return pid;
    return std::nullopt;
}

std::optional<ProcInfo> RequestContextHelper::readProc(qint64 pid) {
    ProcInfo info;
    info.pid = pid;

    // Exe
    info.exe = QFileInfo(QString("/proc/%1/exe").arg(pid)).symLinkTarget();
    if (info.exe.isEmpty())
        return std::nullopt;

    // Cmdline
    QFile fCmd(QString("/proc/%1/cmdline").arg(pid));
    if (fCmd.open(QIODevice::ReadOnly)) {
        QByteArray data = fCmd.readAll();
        fCmd.close();
        // NUL-separated to space-separated
        QList<QByteArray> args = data.split('\0');
        QStringList       cleanArgs;
        for (const auto& a : args)
            if (!a.isEmpty())
                cleanArgs << QString::fromUtf8(a);
        info.cmdline = cleanArgs.join(" ");
    }

    // Status (PPid, Uid)
    QFile fStat(QString("/proc/%1/status").arg(pid));
    if (fStat.open(QIODevice::ReadOnly)) {
        QTextStream ts(&fStat);
        while (!ts.atEnd()) {
            QString line = ts.readLine();
            if (line.startsWith("PPid:")) {
                info.ppid = line.section(':', 1).trimmed().toLongLong();
            } else if (line.startsWith("Uid:")) {
                // Uid: 1000 1000 1000 1000
                info.uid = line.section(':', 1).trimmed().section('\t', 0, 0).toLongLong();
            }
        }
        fStat.close();
    }

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

    return {};
}

ActorInfo RequestContextHelper::resolveRequestorFromSubject(const ProcInfo& subject, qint64 agentUid) {
    ActorInfo actor;
    actor.proc = subject;

    qint64 currPid = subject.pid;
    int    hops    = 0;

    while (currPid > 1 && hops < 16) {
        auto info = readProc(currPid);
        if (!info)
            break;

        // Skip processes not owned by the user (agent) to avoid "systemd" or "root" being the requestor
        if (info->uid != agentUid && agentUid != 0) {
            // If we are root agent (unlikely here but still), we might not want to skip.
            // But usually this agent runs as user.
            break;
        }

        DesktopInfo d = findDesktopForExe(info->exe);
        if (d.isValid()) {
            actor.proc       = *info;
            actor.desktop    = d;
            actor.confidence = "desktop";
            break;
        }

        if (info->ppid <= 1 || info->ppid == currPid)
            break;
        currPid = info->ppid;
        hops++;
    }

    if (!actor.desktop.isValid()) {
        actor.confidence = actor.proc.exe.isEmpty() ? "unknown" : "exe-only";
    }

    // Fill display names
    if (actor.desktop.isValid()) {
        actor.displayName = actor.desktop.name;
        actor.iconName    = actor.desktop.iconName;
    } else if (!actor.proc.exe.isEmpty()) {
        actor.displayName = QFileInfo(actor.proc.exe).fileName();
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

QJsonObject RequestContextHelper::classifyRequest(const QString& source, const QString& title, const QString& description, const ActorInfo& requestor) {
    QJsonObject hint;
    QString     kind     = "unknown";
    QString     icon     = "";
    bool        colorize = false;

    if (source == "polkit") {
        kind     = "polkit";
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
