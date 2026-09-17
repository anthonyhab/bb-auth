#include "../src/core/agent/IntentStore.hpp"
#include <QtTest/QtTest>

using namespace bb::agent;

class IntentStoreTest : public QObject {
    Q_OBJECT

  private:
    qint64 m_now = 1000;

    PendingIntent make(qint64 agentPid, qint64 startTime, const QString& reason) {
        PendingIntent i;
        i.reason         = reason;
        i.declaredAgent  = "claude-code";
        i.channel        = "hook";
        i.agentRootPid   = agentPid;
        i.agentStartTime = startTime;
        return i;
    }

  private slots:
    void correlatesByAgentAndStartTime();
    void startTimeMismatchFailsClosed();
    void wrongAgentPidNoMatch();
    void consumeIsOneShot();
    void expiresAfterTtl();
    void latestDeclarationWinsPerAgent();
    void unboundIntentIgnored();
};

// @lat: [[tests#Intent correlation#Correlates by resolved agent and start-time]]
void IntentStoreTest::correlatesByAgentAndStartTime() {
    IntentStore store([this] { return m_now; });
    store.declare(make(101, 55555, "remove duplicate desktop file"), 20000);

    auto got = store.consumeForAgent(101, 55555);
    QVERIFY(got.has_value());
    QCOMPARE(got->reason, QString("remove duplicate desktop file"));
}

// @lat: [[tests#Intent correlation#Start-time mismatch fails closed]]
void IntentStoreTest::startTimeMismatchFailsClosed() {
    // Same pid number, different generation (pid reuse) must NOT match.
    IntentStore store([this] { return m_now; });
    store.declare(make(101, 55555, "legit"), 20000);

    QVERIFY(!store.consumeForAgent(101, 99999).has_value());
    // The original is still present for the correct generation.
    QVERIFY(store.consumeForAgent(101, 55555).has_value());
}

void IntentStoreTest::wrongAgentPidNoMatch() {
    IntentStore store([this] { return m_now; });
    store.declare(make(101, 55555, "legit"), 20000);
    QVERIFY(!store.consumeForAgent(202, 55555).has_value());
}

// @lat: [[tests#Intent correlation#Consume is one-shot]]
void IntentStoreTest::consumeIsOneShot() {
    IntentStore store([this] { return m_now; });
    store.declare(make(101, 55555, "once"), 20000);
    QVERIFY(store.consumeForAgent(101, 55555).has_value());
    QVERIFY(!store.consumeForAgent(101, 55555).has_value());
}

void IntentStoreTest::expiresAfterTtl() {
    IntentStore store([this] { return m_now; });
    store.declare(make(101, 55555, "stale"), 5000); // expires at 6000
    m_now = 7000;
    QVERIFY(!store.consumeForAgent(101, 55555).has_value());
    QCOMPARE(store.size(), 0);
}

void IntentStoreTest::latestDeclarationWinsPerAgent() {
    IntentStore store([this] { return m_now; });
    store.declare(make(101, 55555, "first"), 20000);
    m_now += 10;
    store.declare(make(101, 55555, "second"), 20000);

    QCOMPARE(store.size(), 1); // same generation replaced, not accumulated
    auto got = store.consumeForAgent(101, 55555);
    QVERIFY(got.has_value());
    QCOMPARE(got->reason, QString("second"));
}

// @lat: [[tests#Intent correlation#Unbound intent ignored]]
void IntentStoreTest::unboundIntentIgnored() {
    IntentStore store([this] { return m_now; });
    store.declare(make(0, 0, "no agent binding"), 20000);
    QCOMPARE(store.size(), 0);
}

int runIntentStoreTests(int argc, char** argv) {
    IntentStoreTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_intent_store.moc"
