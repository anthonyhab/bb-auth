#include "../src/core/RequestContext.hpp"
#include <QtTest/QtTest>
#include <unistd.h>

class RequestContextTest : public QObject {
    Q_OBJECT

  private slots:
    void testSpoofedProcessName();
    void testUnreadableExeSpoofingAttempt();
    void testRealPkexecFallback();
    void testAgentDetectedFromCmdline();
    void testAgentWinsOverAncestry();
    void testNonAgentNotFlagged();
    void testReadProcSelf();
};

void RequestContextTest::testSpoofedProcessName() {
    // PID 101: Trusted Parent (UID 1000)
    ProcInfo trusted;
    trusted.pid = 101;
    trusted.ppid = 1;
    trusted.uid = 1000;
    trusted.euid = 1000;
    trusted.name = "session";
    trusted.exe = "/usr/bin/session";

    // PID 100: Malicious (UID 1001). Spoofed "pkexec".
    // Exe is readable.
    ProcInfo malicious;
    malicious.pid = 100;
    malicious.ppid = 101;
    malicious.uid = 1001; // Different user!
    malicious.euid = 1001;
    malicious.name = "pkexec";
    malicious.exe = "/tmp/malicious";

    auto procReader = [&](qint64 pid) -> std::optional<ProcInfo> {
        if (pid == 100) return malicious;
        if (pid == 101) return trusted;
        return std::nullopt;
    };

    ActorInfo result = RequestContextHelper::resolveRequestorFromSubject(malicious, 1000, procReader);

    // If vulnerable: 'pkexec' spoof allows traversing UID mismatch (1001 != 1000).
    // Reaches 'trusted' (UID 1000).
    // Result: 101.

    // If fixed: Spoof detected. isBridge=false.
    // Stops at malicious (UID mismatch).
    // Result: 100.
    QCOMPARE(result.proc.pid, 100);
}

void RequestContextTest::testUnreadableExeSpoofingAttempt() {
    // PID 101: Trusted Parent (UID 1000)
    ProcInfo trusted;
    trusted.pid = 101;
    trusted.ppid = 1;
    trusted.uid = 1000;
    trusted.euid = 1000;
    trusted.name = "session";
    trusted.exe = "/usr/bin/session";

    // PID 100: Malicious (UID 1001). Spoofed "pkexec".
    // Exe is UNREADABLE.
    // EUID is 1001 (User).
    ProcInfo malicious;
    malicious.pid = 100;
    malicious.ppid = 101;
    malicious.uid = 1001; // Different user
    malicious.euid = 1001;
    malicious.name = "pkexec";
    malicious.exe = "";

    auto procReader = [&](qint64 pid) -> std::optional<ProcInfo> {
        if (pid == 100) return malicious;
        if (pid == 101) return trusted;
        return std::nullopt;
    };

    ActorInfo result = RequestContextHelper::resolveRequestorFromSubject(malicious, 1000, procReader);

    // If fixed: Spoof detected (EUID != 0). isBridge=false.
    // Stops at malicious (UID mismatch).
    // Result: 100.
    QCOMPARE(result.proc.pid, 100);
}

void RequestContextTest::testRealPkexecFallback() {
    // PID 101: Invoking Shell (UID 1000)
    ProcInfo shell;
    shell.pid = 101;
    shell.ppid = 1;
    shell.uid = 1000;
    shell.euid = 1000;
    shell.name = "bash";
    shell.exe = "/usr/bin/bash";

    // PID 100: Real pkexec (Setuid Root)
    // Exe is UNREADABLE.
    // EUID is 0.
    ProcInfo pkexec;
    pkexec.pid = 100;
    pkexec.ppid = 101;
    pkexec.uid = 1000; // RUID=User
    pkexec.euid = 0;   // EUID=Root
    pkexec.name = "pkexec";
    pkexec.exe = "";

    auto procReader = [&](qint64 pid) -> std::optional<ProcInfo> {
        if (pid == 100) return pkexec;
        if (pid == 101) return shell;
        return std::nullopt;
    };

    ActorInfo result = RequestContextHelper::resolveRequestorFromSubject(pkexec, 1000, procReader);

    // Should identify pkexec as bridge (EUID=0, name=pkexec).
    // Should pass through pkexec (even if UID mismatch, though here RUID=1000 so it matches).
    // Continues to shell.
    // Result: 101.
    QCOMPARE(result.proc.pid, 101);
}

void RequestContextTest::testAgentDetectedFromCmdline() {
    // detectAgent matches on cmdline tokens even when exe is a generic runtime (node).
    ProcInfo node;
    node.name    = "node";
    node.exe     = "/usr/bin/node";
    node.cmdline = "node /usr/lib/node_modules/@anthropic-ai/claude-code/cli.js";

    const AgentMatch m = RequestContextHelper::detectAgent(node);
    QVERIFY(m.isValid());
    QCOMPARE(m.kind, QString("claude-code"));

    ProcInfo plain;
    plain.name    = "node";
    plain.exe     = "/usr/bin/node";
    plain.cmdline = "node server.js";
    QVERIFY(!RequestContextHelper::detectAgent(plain).isValid());

    // Test agy agent matching
    ProcInfo agyProc;
    agyProc.name = "agy";
    agyProc.exe = "/home/habibe/.local/bin/agy";
    agyProc.cmdline = "agy";
    const AgentMatch m2 = RequestContextHelper::detectAgent(agyProc);
    QVERIFY(m2.isValid());
    QCOMPARE(m2.kind, QString("gemini-cli"));

    // Test collision resistance (e.g. strategy should not match agy)
    ProcInfo strategyProc;
    strategyProc.name = "strategy";
    strategyProc.exe = "/usr/bin/strategy";
    strategyProc.cmdline = "strategy --run";
    QVERIFY(!RequestContextHelper::detectAgent(strategyProc).isValid());

    // Devin agent matching
    ProcInfo devinProc;
    devinProc.name    = "devin";
    devinProc.exe     = "/opt/devin-desktop/devin";
    devinProc.cmdline = "/opt/devin-desktop/devin --flag";
    const AgentMatch m3 = RequestContextHelper::detectAgent(devinProc);
    QVERIFY(m3.isValid());
    QCOMPARE(m3.kind, QString("devin"));

    // Lookalike names must not match: token/segment equality, not substring —
    // `vim devin-notes.md` and `agyx` are not agents.
    ProcInfo editorProc;
    editorProc.name    = "vim";
    editorProc.exe     = "/usr/bin/vim";
    editorProc.cmdline = "vim /home/user/devin-notes.md";
    QVERIFY(!RequestContextHelper::detectAgent(editorProc).isValid());

    ProcInfo agyx;
    agyx.name    = "agyx";
    agyx.exe     = "/usr/local/bin/agyx";
    agyx.cmdline = "agyx --serve";
    QVERIFY(!RequestContextHelper::detectAgent(agyx).isValid());

    // A real install dir still resolves: the alias is a path segment.
    ProcInfo optClaude;
    optClaude.name    = "node";
    optClaude.exe     = "/usr/bin/node";
    optClaude.cmdline = "node /opt/claude-code/cli.js --print";
    const AgentMatch m4 = RequestContextHelper::detectAgent(optClaude);
    QVERIFY(m4.isValid());
    QCOMPARE(m4.kind, QString("claude-code"));
}

void RequestContextTest::testAgentWinsOverAncestry() {
    // pkexec (root bridge) -> node(claude) -> would-be terminal.
    // Resolution must stop at the agent and attribute it, not walk to the terminal.
    ProcInfo pkexec;
    pkexec.pid = 100;
    pkexec.ppid = 101;
    pkexec.uid = 1000;
    pkexec.euid = 0;
    pkexec.name = "pkexec";
    pkexec.exe = "";

    ProcInfo claude;
    claude.pid = 101;
    claude.ppid = 102;
    claude.uid = 1000;
    claude.euid = 1000;
    claude.name = "node";
    claude.exe = "/usr/bin/node";
    claude.cmdline = "node /opt/claude-code/cli.js --print";

    ProcInfo terminal;
    terminal.pid = 102;
    terminal.ppid = 1;
    terminal.uid = 1000;
    terminal.euid = 1000;
    terminal.name = "ghostty";
    terminal.exe = "/usr/bin/ghostty";

    auto procReader = [&](qint64 pid) -> std::optional<ProcInfo> {
        if (pid == 100) return pkexec;
        if (pid == 101) return claude;
        if (pid == 102) return terminal;
        return std::nullopt;
    };

    ActorInfo result = RequestContextHelper::resolveRequestorFromSubject(pkexec, 1000, procReader);
    QVERIFY(result.isAgent);
    QCOMPARE(result.agentKind, QString("claude-code"));
    QCOMPARE(result.proc.pid, (qint64)101);
    QCOMPARE(result.confidence, QString("agent"));
}

void RequestContextTest::testNonAgentNotFlagged() {
    // pkexec -> bash, no agent anywhere. Must not be flagged as an agent.
    ProcInfo pkexec;
    pkexec.pid = 100;
    pkexec.ppid = 101;
    pkexec.uid = 1000;
    pkexec.euid = 0;
    pkexec.name = "pkexec";
    pkexec.exe = "";

    ProcInfo bash;
    bash.pid = 101;
    bash.ppid = 1;
    bash.uid = 1000;
    bash.euid = 1000;
    bash.name = "bash";
    bash.exe = "/usr/bin/bash";
    bash.cmdline = "bash";

    auto procReader = [&](qint64 pid) -> std::optional<ProcInfo> {
        if (pid == 100) return pkexec;
        if (pid == 101) return bash;
        return std::nullopt;
    };

    ActorInfo result = RequestContextHelper::resolveRequestorFromSubject(pkexec, 1000, procReader);
    QVERIFY(!result.isAgent);
    QVERIFY(result.agentKind.isEmpty());
}

void RequestContextTest::testReadProcSelf() {
    // Exercise the real dirfd-pinned /proc reader against our own process.
    const auto info = RequestContextHelper::readProc(getpid());
    QVERIFY(info.has_value());
    QCOMPARE(info->pid, (qint64)getpid());
    QCOMPARE(info->ppid, (qint64)getppid());
    QVERIFY(!info->name.isEmpty());
    QVERIFY(info->startTime > 0);

    // A pid that cannot exist must fail closed, not return garbage.
    QVERIFY(!RequestContextHelper::readProc(2147483646).has_value());
}

// We need an entry point.
int runRequestContextTests(int argc, char** argv) {
    RequestContextTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_request_context.moc"
