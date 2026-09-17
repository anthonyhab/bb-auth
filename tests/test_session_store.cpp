#include "../src/core/agent/SessionStore.hpp"
#include <QtTest/QtTest>

namespace bb {

class SessionStoreTest : public QObject {
    Q_OBJECT

  private slots:
    void createSession_rejectsDuplicateId();
    void createSession_rejectsDuplicateIdAcrossSources();
    void rapidChurn_keepsStoreConsistent();
    void queuedSessions_drainCompletely();
};

void SessionStoreTest::createSession_rejectsDuplicateId() {
    agent::SessionStore store;
    const QString id = "test-session";

    // Create first session
    auto result1 = store.createSession(id, Session::Source::Polkit, Session::Context{});
    QVERIFY(result1.has_value());
    QCOMPARE(store.size(), 1);

    // Create second session with same ID - this should fail now
    auto result2 = store.createSession(id, Session::Source::Polkit, Session::Context{});
    QVERIFY(!result2.has_value());
    QCOMPARE(store.size(), 1);
}

void SessionStoreTest::createSession_rejectsDuplicateIdAcrossSources() {
    agent::SessionStore store;
    const QString       id = "shared-session-id";

    auto result1 = store.createSession(id, Session::Source::Polkit, Session::Context{});
    QVERIFY(result1.has_value());
    QCOMPARE(store.size(), 1);

    auto result2 = store.createSession(id, Session::Source::Pinentry, Session::Context{});
    QVERIFY(!result2.has_value());
    QCOMPARE(store.size(), 1);
}

void SessionStoreTest::rapidChurn_keepsStoreConsistent() {
    agent::SessionStore store;

    // Rapid create→update→close churn across all sources: the store must end
    // empty and every operation must return a structured result — the prompt
    // churn a hostile or flaky requestor can induce.
    const Session::Source sources[] = {Session::Source::Polkit, Session::Source::Keyring, Session::Source::Pinentry};
    for (int i = 0; i < 3000; ++i) {
        const QString id = QStringLiteral("churn-%1").arg(i);
        QVERIFY(store.createSession(id, sources[i % 3], Session::Context{}).has_value());
        QVERIFY(store.updatePrompt(id, QStringLiteral("Password:"), false, true).has_value());
        QVERIFY(store.closeSession(id, (i % 2) ? Session::Result::Success : Session::Result::Cancelled).has_value());
        QVERIFY(store.getSession(id) == nullptr);
    }
    QVERIFY(store.empty());
    QCOMPARE(store.size(), 0);
}

void SessionStoreTest::queuedSessions_drainCompletely() {
    agent::SessionStore store;

    // Many sessions open at once (queueing), then all close — no leaks, and
    // closing an already-closed id fails cleanly.
    for (int i = 0; i < 500; ++i)
        QVERIFY(store.createSession(QStringLiteral("queued-%1").arg(i), Session::Source::Polkit, Session::Context{}).has_value());
    QCOMPARE(store.size(), 500);

    for (int i = 0; i < 500; ++i)
        QVERIFY(store.closeSession(QStringLiteral("queued-%1").arg(i), Session::Result::Success).has_value());
    QVERIFY(store.empty());
    QVERIFY(!store.closeSession(QStringLiteral("queued-0"), Session::Result::Success).has_value());
}

} // namespace bb

int runSessionStoreTests(int argc, char** argv) {
    bb::SessionStoreTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_session_store.moc"
