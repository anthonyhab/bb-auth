#include "../src/core/Session.hpp"

#include <QtTest/QtTest>
#include <QApplication>

int runFallbackWindowTouchModelTests(int argc, char** argv);
int runAgentRoutingTests(int argc, char** argv);

class SessionInfoTest : public QObject {
    Q_OBJECT

  private slots:
    void toUpdatedEventIncludesInfoAfterSetInfo();
    void setPromptClearsStaleInfo();
    void updatedEventCanContainErrorAndInfo();

  private:
    static bb::Session makePolkitSession();
};

bb::Session SessionInfoTest::makePolkitSession() {
    bb::Session::Context context;
    context.message        = "Authenticate to continue";
    context.requestor.name = "test-app";
    return bb::Session("session-1", bb::Session::Source::Polkit, context);
}

void SessionInfoTest::toUpdatedEventIncludesInfoAfterSetInfo() {
    bb::Session session = makePolkitSession();

    session.setPrompt("Password:", false);
    session.setInfo("Touch your security key");

    const QJsonObject event = session.toUpdatedEvent();
    QVERIFY(event.contains("info"));
    QCOMPARE(event.value("info").toString(), QString("Touch your security key"));
}

void SessionInfoTest::setPromptClearsStaleInfo() {
    bb::Session session = makePolkitSession();

    session.setPrompt("Password:", false);
    session.setInfo("Scan your finger");
    session.setPrompt("Password:", false);

    const QJsonObject event = session.toUpdatedEvent();
    QVERIFY(!event.contains("info"));
}

void SessionInfoTest::updatedEventCanContainErrorAndInfo() {
    bb::Session session = makePolkitSession();

    session.setPrompt("Password:", false);
    session.setError("Authentication failed");
    session.setInfo("Touch your security key");

    const QJsonObject event = session.toUpdatedEvent();
    QVERIFY(event.contains("error"));
    QVERIFY(event.contains("info"));
    QCOMPARE(event.value("error").toString(), QString("Authentication failed"));
    QCOMPARE(event.value("info").toString(), QString("Touch your security key"));
}

int main(int argc, char** argv) {
    QApplication    app(argc, argv);
    SessionInfoTest sessionInfoTest;
    const int       sessionResult  = QTest::qExec(&sessionInfoTest, argc, argv);
    const int       routingResult  = runAgentRoutingTests(argc, argv);
    const int       fallbackResult = runFallbackWindowTouchModelTests(argc, argv);
    if (sessionResult != 0) {
        return sessionResult;
    }
    if (routingResult != 0) {
        return routingResult;
    }
    return fallbackResult;
}

#include "test_session_info.moc"
