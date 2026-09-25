#pragma once

#include <QString>
#include <QJsonObject>
#include <optional>
#include <functional>
#include <polkitqt1-details.h>

#include "../common/ProcAgent.hpp"

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
    // Agent attribution (OS-resolved — the trust anchor). When isAgent is true,
    // proc.pid is the recognized agent process and agentKind names it.
    bool        isAgent{false};
    QString     agentKind;

    QJsonObject toJson() const;
};

class RequestContextHelper {
  public:
    static std::optional<qint64>   extractSubjectPid(const PolkitQt1::Details& details);
    static std::optional<qint64>   extractCallerPid(const PolkitQt1::Details& details);
    static std::optional<ProcInfo> readProc(qint64 pid) {
        return bb::readProc(pid);
    }
    static DesktopInfo             findDesktopForExe(const QString& exePath);
    // Recognize a known AI agent runtime from a process's exe/cmdline. Returns an
    // invalid match when the process is not a recognized agent.
    static AgentMatch detectAgent(const ProcInfo& proc) {
        return bb::detectAgent(proc);
    }
    static ActorInfo               resolveRequestorFromSubject(const ProcInfo& subject, qint64 agentUid);
    static ActorInfo               resolveRequestorFromSubject(const ProcInfo& subject, qint64 agentUid, std::function<std::optional<ProcInfo>(qint64)> procReader);
    static QString                 normalizePrompt(QString s);
    static QJsonObject             classifyRequest(const QString& source, const QString& title, const QString& description);

  private:
    static void ensureDesktopIndex();
};
