#include "../src/core/agent/ProviderTrustStore.hpp"

#include <QtTest/QtTest>

#include <QHash>
#include <QProcess>
#include <QStandardPaths>

namespace bb {

    class ProviderTrustStoreTest : public QObject {
        Q_OBJECT

      private slots:
        void trustsLaunchedPeerWithMatchingStartTime();
        void rejectsUnknownPeer();
        void rejectsRecycledPidWithDifferentStartTime();
        void trustIsSingleUse();
        void ignoresLaunchWithUnreadableStartTime();
        void prunesAbandonedLaunchesAfterTtl();
        void trustsRealLaunchedProcessViaProc();
    };

    namespace {

        // A fake /proc start-time source: maps pid -> start-time, 0 (unreadable) for the rest.
        struct FakeStartTimes {
            QHash<qint64, qint64> byPid;
            qint64                operator()(qint64 pid) const {
                return byPid.value(pid, 0);
            }
        };

    } // namespace

    void ProviderTrustStoreTest::trustsLaunchedPeerWithMatchingStartTime() {
        qint64        nowMs = 1000;
        FakeStartTimes starts{{{4242, 555}}};
        agent::ProviderTrustStore store([&nowMs] { return nowMs; }, [&starts](qint64 pid) { return starts(pid); });

        store.recordLaunch(4242);
        QCOMPARE(store.pendingCount(), 1);
        QVERIFY(store.consumeTrust(4242));
    }

    void ProviderTrustStoreTest::rejectsUnknownPeer() {
        qint64        nowMs = 1000;
        FakeStartTimes starts{{{4242, 555}, {9999, 777}}};
        agent::ProviderTrustStore store([&nowMs] { return nowMs; }, [&starts](qint64 pid) { return starts(pid); });

        store.recordLaunch(4242);
        // A peer the daemon never launched is not trusted, even if its /proc is readable.
        QVERIFY(!store.consumeTrust(9999));
        QVERIFY(!store.consumeTrust(-1));
        QVERIFY(!store.consumeTrust(0));
    }

    void ProviderTrustStoreTest::rejectsRecycledPidWithDifferentStartTime() {
        qint64        nowMs = 1000;
        FakeStartTimes starts{{{4242, 555}}};
        agent::ProviderTrustStore store([&nowMs] { return nowMs; }, [&starts](qint64 pid) { return starts(pid); });

        store.recordLaunch(4242); // recorded with start-time 555

        // The launched process died and pid 4242 was recycled by another process with a
        // later start-time. The pid matches but the generation does not → not trusted.
        starts.byPid[4242] = 556;
        QVERIFY(!store.consumeTrust(4242));
    }

    void ProviderTrustStoreTest::trustIsSingleUse() {
        qint64        nowMs = 1000;
        FakeStartTimes starts{{{4242, 555}}};
        agent::ProviderTrustStore store([&nowMs] { return nowMs; }, [&starts](qint64 pid) { return starts(pid); });

        store.recordLaunch(4242);
        QVERIFY(store.consumeTrust(4242));
        // A launch authorizes exactly one registration; a second attempt finds nothing.
        QVERIFY(!store.consumeTrust(4242));
        QCOMPARE(store.pendingCount(), 0);
    }

    void ProviderTrustStoreTest::ignoresLaunchWithUnreadableStartTime() {
        qint64        nowMs = 1000;
        FakeStartTimes starts; // pid 4242 has no entry → start-time reads 0
        agent::ProviderTrustStore store([&nowMs] { return nowMs; }, [&starts](qint64 pid) { return starts(pid); });

        // Without a start-time the record cannot defend against pid reuse, so it is dropped.
        store.recordLaunch(4242);
        QCOMPARE(store.pendingCount(), 0);
        QVERIFY(!store.consumeTrust(4242));
    }

    void ProviderTrustStoreTest::prunesAbandonedLaunchesAfterTtl() {
        qint64        nowMs = 1000;
        FakeStartTimes starts{{{4242, 555}}};
        agent::ProviderTrustStore store([&nowMs] { return nowMs; }, [&starts](qint64 pid) { return starts(pid); });

        store.recordLaunch(4242);
        QCOMPARE(store.pendingCount(), 1);

        // A provider that never connects within the TTL is abandoned and pruned, so the
        // map cannot grow without bound on repeated relaunches.
        nowMs += 31000;
        QVERIFY(!store.consumeTrust(4242));
        QCOMPARE(store.pendingCount(), 0);
    }

    void ProviderTrustStoreTest::trustsRealLaunchedProcessViaProc() {
        // Integration: this is the production stranding-risk path. Launch a real process
        // exactly as ProviderLauncher does (startDetached(&pid)), then attest it through
        // the DEFAULT store (real /proc start-time reader). If startDetached's pid did not
        // match the live process's /proc start-time, the daemon's own fallback would be
        // denied and every auth prompt would strand — so this must hold.
        const QString sleepBin = QStandardPaths::findExecutable("sleep");
        if (sleepBin.isEmpty()) {
            QSKIP("no 'sleep' binary available to launch a real process");
        }

        QProcess process;
        process.setProgram(sleepBin);
        process.setArguments({"3"});
        qint64 pid = 0;
        QVERIFY2(process.startDetached(&pid), "startDetached failed");
        QVERIFY2(pid > 0, "startDetached returned no pid");

        agent::ProviderTrustStore store; // default: real clock + /proc start-time reader
        store.recordLaunch(pid);
        QCOMPARE(store.pendingCount(), 1);
        QVERIFY2(store.consumeTrust(pid), "real launched process was not attested via /proc");

        // A pid the daemon did not launch is never trusted, even though it is live.
        QVERIFY(!store.consumeTrust(static_cast<qint64>(QCoreApplication::applicationPid())));

        process.kill();
    }

} // namespace bb

int runProviderTrustTests(int argc, char** argv) {
    bb::ProviderTrustStoreTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_provider_trust.moc"
