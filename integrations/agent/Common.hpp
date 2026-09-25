#pragma once

// Shared plumbing for bb-auth-agent, the multi-call binary behind
// bb-auth-intent-hook, aisudo, the sudo/doas/pkexec PATH shims, and
// bb-auth-agents. Qt6::Core only; every path here fails open.

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace bb::agent {

    inline constexpr int kDeclareTtlMs = 20000;
    inline constexpr int kMaxReason    = 2000;

    // Daemon reply to intent.declare. `answered` means the daemon replied
    // {"type":"ok","bound":<bool>} — proof the supervised path is live.
    struct DeclareResult {
        bool answered = false;
        bool bound    = false;
    };

    QString       socketPath();
    // Bounded (500 ms) request/reply over the daemon socket; never throws,
    // never blocks longer than the bound.
    DeclareResult declareIntent(const QJsonObject &payload);
    // Liveness ping for `bb-auth-agents status`.
    bool          daemonPing();

    // pkcheck exit code for org.freedesktop.policykit.exec on this process,
    // without user interaction: 0 authorized, 1 denied, 2 challenge, other on
    // error; -1 when pkcheck could not run or timed out (1 s).
    int  pkcheckExec();
    // True only when polkit will challenge the user (pkcheck exit 2).
    bool polkitWillChallenge();

    // Agent kind of the nearest recognized agent ancestor (same uid only,
    // 16 hops), or empty when not running under one.
    QString detectAgentAncestor();

    // Unbuffered writes; errors ignored (a closed pipe must not crash us).
    void writeOut(const QByteArray &bytes);
    void writeErr(const QByteArray &bytes);

    // execv/execvp wrappers taking QStringList argv; return only on failure.
    void execPath(const QString &path, const QStringList &argv);
    void execSearch(const QStringList &argv);

    // Build-time paths (configure_file'd), overridable by env for tests.
    QString hookPath();
    QString integrationsDir();
    QString shimDir();

} // namespace bb::agent
