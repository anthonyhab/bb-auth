#pragma once

// Process introspection and AI-agent recognition shared by the daemon and the
// agent tools (bb-auth-agent). Qt6::Core only — no polkit, no GUI — so the
// agent binary can link it without pulling in the daemon's dependencies.

#include <QJsonObject>
#include <QString>
#include <optional>

struct ProcInfo {
    qint64      pid       = 0;
    qint64      ppid      = 0;
    qint64      uid       = 0;
    qint64      euid      = 0;
    qint64      startTime = 0; // /proc/<pid>/stat field 22 (clock ticks since boot)
    QString     name;
    QString     exe;
    QString     cmdline;

    QJsonObject toJson() const;
};

// OS-resolved recognition of a known AI agent runtime. Empty kind == not an agent.
struct AgentMatch {
    QString kind;        // stable id, e.g. "claude-code"
    QString displayName; // friendly, e.g. "Claude Code"
    QString iconName;    // icon hint for the UI

    bool    isValid() const {
        return !kind.isEmpty();
    }
};

namespace bb {

    // Read /proc/<pid> pinned by a directory fd so every field describes the
    // same process generation. nullopt when the pid is gone or unreadable.
    std::optional<ProcInfo> readProc(qint64 pid);

    // Recognize a known AI agent runtime from a process's comm/exe/cmdline.
    AgentMatch detectAgent(const ProcInfo& proc);

} // namespace bb
