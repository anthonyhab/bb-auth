#include "../src/core/RequestContext.hpp"
#include <QtTest/QtTest>

class RequestContextTest : public QObject {
    Q_OBJECT

  private slots:
    void testSpoofedProcessName();
    void testUnreadableExeSpoofingAttempt();
    void testRealPkexecFallback();
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

// We need an entry point.
int runRequestContextTests(int argc, char** argv) {
    RequestContextTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_request_context.moc"
