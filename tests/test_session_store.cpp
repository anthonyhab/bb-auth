#include "../src/core/agent/SessionStore.hpp"
#include <QtTest/QtTest>

namespace bb {

class SessionStoreTest : public QObject {
    Q_OBJECT

  private slots:
    void createSession_rejectsDuplicateId();
    void createSession_rejectsDuplicateIdAcrossSources();
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

} // namespace bb

int runSessionStoreTests(int argc, char** argv) {
    bb::SessionStoreTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_session_store.moc"
