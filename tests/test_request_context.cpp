#include "../src/core/RequestContext.hpp"
#include "../src/core/RequestFacts.hpp"
#include <QDir>
#include <QJsonArray>
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
    void testFactsRequireRootCaller();
    void testFactsPkexecKeepCwd();
    void testFactsPkexecRunsInTargetHome();
    void testFactsRejectMismatchedCommandLine();
    void testFactsRejectNonPkexecCaller();
    void testFactsCanonicalProgram();
    void testFactsKeepEmptyArguments();
    void testFactsBoundArgv();
    void testFactsFromPkexecMessage();
    void testFactsFromShortenedPkexecMessage();
    void testFactsRejectMismatchedMessage();
    void testFactsMechanismDetails();
    void testReadProcArgvAndCwdSelf();
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

    // pi matches on its npm package dir segment (`node …/pi-coding-agent/…`);
    // a bare `pi` process must NOT match — the name collides with the Debian
    // `pi` utility.
    ProcInfo piProc;
    piProc.name    = "node";
    piProc.exe     = "/usr/bin/node";
    piProc.cmdline = "node /usr/lib/node_modules/@earendil-works/pi-coding-agent/dist/cli.js";
    const AgentMatch m5 = RequestContextHelper::detectAgent(piProc);
    QVERIFY(m5.isValid());
    QCOMPARE(m5.kind, QString("pi"));

    ProcInfo barePi;
    barePi.name    = "pi";
    barePi.exe     = "/usr/bin/pi";
    barePi.cmdline = "pi 1000000";
    QVERIFY(!RequestContextHelper::detectAgent(barePi).isValid());
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
namespace {
    ProcInfo pkexecCaller(const QStringList& argv, qint64 euid = 0) {
        ProcInfo p;
        p.pid  = 4242;
        p.uid  = 1000;
        p.euid = euid;
        p.name = "pkexec";
        p.argv = argv;
        return p;
    }

    bb::RequestFactsInput execInput(const QStringList& argv, const QString& program, const QString& commandLine) {
        bb::RequestFactsInput in;
        in.actionId = "org.freedesktop.policykit.exec";
        in.details  = {{"program", program}, {"command_line", commandLine}, {"user", "root"}, {"polkit.caller-pid", "4242"}};
        in.caller   = pkexecCaller(argv);
        in.subjectCwd  = "/home/u/proj";
        in.homeForUser = [](const QString& user) { return user == "root" ? QString("/root") : QString(); };
        return in;
    }
}

void RequestContextTest::testFactsRequireRootCaller() {
    // A user process can call polkit with any details it likes.
    auto in   = execInput({"pkexec", "rm", "x"}, "/usr/bin/rm", "/usr/bin/rm x");
    in.caller = pkexecCaller({"pkexec", "rm", "x"}, 1000);
    QVERIFY(bb::buildRequestFacts(in).isEmpty());
    in.caller.reset();
    QVERIFY(bb::buildRequestFacts(in).isEmpty());
}

void RequestContextTest::testFactsPkexecKeepCwd() {
    const auto facts = bb::buildRequestFacts(execInput({"/usr/bin/pkexec", "--keep-cwd", "rm", "-rf", "build"}, "/usr/bin/rm", "/usr/bin/rm -rf build"));
    QCOMPARE(facts.value("kind").toString(), QString("exec"));
    QCOMPARE(facts.value("program").toString(), QString("/usr/bin/rm"));
    QCOMPARE(facts.value("argv").toArray(), QJsonArray({"/usr/bin/rm", "-rf", "build"}));
    QCOMPARE(facts.value("user").toString(), QString("root"));
    QCOMPARE(facts.value("cwd").toString(), QString("/home/u/proj"));
    QCOMPARE(facts.value("cwdFrom").toString(), QString("requester"));
    QVERIFY(!facts.contains("truncated"));
}

void RequestContextTest::testFactsPkexecRunsInTargetHome() {
    const auto facts = bb::buildRequestFacts(execInput({"pkexec", "rm", "-rf", "build"}, "/usr/bin/rm", "/usr/bin/rm -rf build"));
    QCOMPARE(facts.value("cwd").toString(), QString("/root"));
    QCOMPARE(facts.value("cwdFrom").toString(), QString("target-home"));
}

void RequestContextTest::testFactsRejectMismatchedCommandLine() {
    QVERIFY(bb::buildRequestFacts(execInput({"pkexec", "rm", "a"}, "/usr/bin/rm", "/usr/bin/rm b")).isEmpty());
    QVERIFY(bb::buildRequestFacts(execInput({"pkexec", "rm", "a"}, "rm", "rm a")).isEmpty());
    // A relative PROGRAM must resolve to a path ending in that name.
    QVERIFY(bb::buildRequestFacts(execInput({"pkexec", "rm", "a"}, "/usr/bin/cp", "/usr/bin/cp a")).isEmpty());
    // An absolute PROGRAM appears as written.
    QVERIFY(bb::buildRequestFacts(execInput({"pkexec", "/bin/rm", "a"}, "/usr/bin/rm", "/usr/bin/rm a")).isEmpty());
    QVERIFY(bb::buildRequestFacts(execInput({"pkexec", "--user"}, "/bin/bash", "/bin/bash")).isEmpty());
    // Bare pkexec opens the target user's shell; pkexec reports it as PROGRAM.
    const auto shell = bb::buildRequestFacts(execInput({"pkexec"}, "/usr/bin/bash", "/bin/bash"));
    QCOMPARE(shell.value("argv").toArray(), QJsonArray({"/bin/bash"}));
    QCOMPARE(shell.value("program").toString(), QString("/usr/bin/bash"));
}

void RequestContextTest::testFactsRejectNonPkexecCaller() {
    QVERIFY(bb::buildRequestFacts(execInput({"sudo", "rm", "a"}, "/usr/bin/rm", "/usr/bin/rm a")).isEmpty());
    QVERIFY(bb::buildRequestFacts(execInput({}, "/usr/bin/rm", "/usr/bin/rm a")).isEmpty());
}

void RequestContextTest::testFactsCanonicalProgram() {
    // program is realpath()'d; argv keeps the name the program is called by
    // (busybox-style multi-call binaries dispatch on it).
    const auto facts = bb::buildRequestFacts(execInput({"pkexec", "/bin/rm", "x"}, "/usr/bin/busybox", "/bin/rm x"));
    QCOMPARE(facts.value("program").toString(), QString("/usr/bin/busybox"));
    QCOMPARE(facts.value("argv").toArray(), QJsonArray({"/bin/rm", "x"}));
    const auto viaPath = bb::buildRequestFacts(execInput({"pkexec", "vi", "/etc/hosts"}, "/usr/bin/vim", "/usr/bin/vi /etc/hosts"));
    QCOMPARE(viaPath.value("argv").toArray(), QJsonArray({"/usr/bin/vi", "/etc/hosts"}));
}

void RequestContextTest::testFactsKeepEmptyArguments() {
    const auto facts = bb::buildRequestFacts(execInput({"pkexec", "--keep-cwd", "touch", "", "x"}, "/usr/bin/touch", "/usr/bin/touch  x"));
    QCOMPARE(facts.value("argv").toArray(), QJsonArray({"/usr/bin/touch", "", "x"}));
}

void RequestContextTest::testFactsBoundArgv() {
    QStringList argv{"pkexec", "rm"};
    for (int i = 0; i < 300; ++i)
        argv << QString("f%1").arg(i);
    const QString commandLine = (QStringList{"/usr/bin/rm"} + argv.mid(2)).join(' ');
    const auto    facts       = bb::buildRequestFacts(execInput(argv, "/usr/bin/rm", commandLine));
    QCOMPARE(facts.value("truncated").toBool(), true);
    QCOMPARE(facts.value("argv").toArray().size(), 256);
}

namespace {
    // What polkit 127 actually forwards: no pkexec details, the command in
    // the message, root showing as the overflow uid with full capabilities.
    bb::RequestFactsInput messageInput(const QStringList& argv, const QString& message) {
        bb::RequestFactsInput in;
        in.actionId      = "org.freedesktop.policykit.exec";
        in.message       = message;
        in.details       = {{"polkit.caller-pid", "4242"}, {"polkit.subject-pid", "4241"}};
        in.caller        = pkexecCaller(argv, 65534);
        in.caller->capEff = 0x000001ffffffffffULL;
        in.subjectCwd    = "/home/u/proj";
        in.homeForUser   = [](const QString& user) { return user == "root" ? QString("/root") : QString("/home/") + user; };
        return in;
    }
}

void RequestContextTest::testFactsFromPkexecMessage() {
    const auto facts = bb::buildRequestFacts(messageInput({"pkexec", "rm", "-r", "/tmp/bb-demo"},
                                                          "Authentication is needed to run `/usr/bin/rm -r /tmp/bb-demo' as the super user"));
    QCOMPARE(facts.value("argv").toArray(), QJsonArray({"/usr/bin/rm", "-r", "/tmp/bb-demo"}));
    QCOMPARE(facts.value("program").toString(), QString("/usr/bin/rm"));
    QCOMPARE(facts.value("user").toString(), QString("root"));
    QCOMPARE(facts.value("cwdFrom").toString(), QString("target-home"));

    const auto other = bb::buildRequestFacts(messageInput({"pkexec", "--user", "alice", "--keep-cwd", "/usr/bin/id"},
                                                          "Authentication is needed to run `/usr/bin/id' as user Alice Liddell (alice)"));
    QCOMPARE(other.value("user").toString(), QString("alice"));
    QCOMPARE(other.value("cwdFrom").toString(), QString("requester"));
}

void RequestContextTest::testFactsFromShortenedPkexecMessage() {
    QStringList argv{"pkexec", "pacman", "-S", "--needed"};
    for (int i = 0; i < 12; ++i)
        argv << QString("package-%1").arg(i);
    const QString full  = (QStringList{"/usr/bin/pacman"} + argv.mid(2)).join(' ');
    const QString shown = full.left(38) + " ... " + full.right(37);
    const auto    facts = bb::buildRequestFacts(messageInput(argv, "Authentication is needed to run `" + shown + "' as the super user"));
    QCOMPARE(facts.value("argv").toArray().size(), argv.size() - 1);
    QCOMPARE(facts.value("argv").toArray().first().toString(), QString("/usr/bin/pacman"));

    // The argv comes from the caller's /proc; the message cross-checks the
    // parts it shows, which must match byte for byte.
    QVERIFY(bb::buildRequestFacts(messageInput(argv, "Authentication is needed to run `" + shown.left(shown.size() - 1) + "x' as the super user")).isEmpty());
}

void RequestContextTest::testFactsRejectMismatchedMessage() {
    // The message names another command than the caller's argv.
    QVERIFY(bb::buildRequestFacts(messageInput({"pkexec", "rm", "a"}, "Authentication is needed to run `/usr/bin/rm b' as the super user")).isEmpty());
    // PATH resolution must end in PROGRAM.
    QVERIFY(bb::buildRequestFacts(messageInput({"pkexec", "rm", "a"}, "Authentication is needed to run `/usr/bin/cp a' as the super user")).isEmpty());
    // Not pkexec's wording (another action's or a translated message).
    QVERIFY(bb::buildRequestFacts(messageInput({"pkexec", "rm", "a"}, "Authentifizierung ist erforderlich")).isEmpty());
    // An unprivileged caller: overflow uid without capabilities.
    auto in           = messageInput({"pkexec", "rm", "a"}, "Authentication is needed to run `/usr/bin/rm a' as the super user");
    in.caller->capEff = 0;
    QVERIFY(bb::buildRequestFacts(in).isEmpty());
}

void RequestContextTest::testFactsMechanismDetails() {
    bb::RequestFactsInput in;
    in.actionId = "org.freedesktop.systemd1.manage-units";
    in.details  = {{"unit", "bluetooth.service"}, {"verb", "restart"}, {"polkit.caller-pid", "1"}, {"polkit.message", "x"}};
    ProcInfo systemd;
    systemd.pid  = 1;
    systemd.euid = 0;
    in.caller    = systemd;
    const auto facts = bb::buildRequestFacts(in);
    QCOMPARE(facts.value("kind").toString(), QString("action"));
    const auto details = facts.value("details").toObject();
    QCOMPARE(details.keys(), QStringList({"unit", "verb"}));
    QVERIFY(!facts.contains("argv"));
}

void RequestContextTest::testReadProcArgvAndCwdSelf() {
    const auto self = bb::readProc(getpid());
    QVERIFY(self.has_value());
    QVERIFY(!self->argv.isEmpty());
    QCOMPARE(bb::readProcCwd(getpid()), QDir::currentPath());
}

int runRequestContextTests(int argc, char** argv) {
    RequestContextTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_request_context.moc"
