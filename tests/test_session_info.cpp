#include "../src/core/Session.hpp"

#include <QtTest/QtTest>
#include <QApplication>

int runFallbackWindowTouchModelTests(int argc, char** argv);
int runAgentRoutingTests(int argc, char** argv);
int runProviderManifestTests(int argc, char** argv);
int runProviderDiscoveryTests(int argc, char** argv);
int runProviderLauncherTests(int argc, char** argv);
int runProviderConformanceTests(int argc, char** argv);
int runTextNormalizeTests(int argc, char** argv);
int runSessionStoreTests(int argc, char** argv);
int runClassifyRequestTests(int argc, char** argv);
int runPromptExtractorsTests(int argc, char** argv);
int runRequestContextTests(int argc, char** argv);

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
    const int       sessionResult        = QTest::qExec(&sessionInfoTest, argc, argv);
    const int       routingResult        = runAgentRoutingTests(argc, argv);
    const int       fallbackResult       = runFallbackWindowTouchModelTests(argc, argv);
    const int       storeResult          = runSessionStoreTests(argc, argv);
    const int       classifyResult       = runClassifyRequestTests(argc, argv);
    const int       extractorsResult     = runPromptExtractorsTests(argc, argv);
    const int       requestContextResult = runRequestContextTests(argc, argv);
    const int       normalizeResult      = runTextNormalizeTests(argc, argv);
    const int       manifestResult       = runProviderManifestTests(argc, argv);
    const int       discoveryResult      = runProviderDiscoveryTests(argc, argv);
    const int       launcherResult       = runProviderLauncherTests(argc, argv);
    const int       conformanceResult    = runProviderConformanceTests(argc, argv);
    if (sessionResult != 0) {
        return sessionResult;
    }
    if (routingResult != 0) {
        return routingResult;
    }
    if (fallbackResult != 0) {
        return fallbackResult;
    }
    if (storeResult != 0) {
        return storeResult;
    }
    if (classifyResult != 0) {
        return classifyResult;
    }
    if (extractorsResult != 0) {
        return extractorsResult;
    }
    if (requestContextResult != 0) {
        return requestContextResult;
    }
    if (normalizeResult != 0) {
        return normalizeResult;
    }
    if (manifestResult != 0) {
        return manifestResult;
    }
    if (discoveryResult != 0) {
        return discoveryResult;
    }
    if (launcherResult != 0) {
        return launcherResult;
    }
    return conformanceResult;
}

#include "test_session_info.moc"
