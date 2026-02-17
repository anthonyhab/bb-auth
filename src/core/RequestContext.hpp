#pragma once

#include <QString>
#include <QJsonObject>
#include <optional>
#include <functional>
#include <polkitqt1-details.h>

struct ProcInfo {
    qint64      pid  = 0;
    qint64      ppid = 0;
    qint64      uid  = 0;
    qint64      euid = 0;
    QString     name;
    QString     exe;
    QString     cmdline;

    QJsonObject toJson() const;
};

struct DesktopInfo {
    QString desktopId;
    QString name;
    QString iconName;
    QString exec;
    QString tryExec;

    bool    isValid() const {
        return !desktopId.isEmpty();
    }
};

struct ActorInfo {
    ProcInfo    proc;
    DesktopInfo desktop;
    QString     displayName;
    QString     iconName;
    QString     fallbackLetter;
    QString     fallbackKey;
    QString     confidence;

    QJsonObject toJson() const;
};

class RequestContextHelper {
  public:
    static std::optional<qint64>   extractSubjectPid(const PolkitQt1::Details& details);
    static std::optional<qint64>   extractCallerPid(const PolkitQt1::Details& details);
    static std::optional<ProcInfo> readProc(qint64 pid);
    static DesktopInfo             findDesktopForExe(const QString& exePath);
    static ActorInfo               resolveRequestorFromSubject(const ProcInfo& subject, qint64 agentUid);
    static ActorInfo               resolveRequestorFromSubject(const ProcInfo& subject, qint64 agentUid, std::function<std::optional<ProcInfo>(qint64)> procReader);
    static QString                 normalizePrompt(QString s);
    static QJsonObject             classifyRequest(const QString& source, const QString& title, const QString& description);

  private:
    static void ensureDesktopIndex();
};
