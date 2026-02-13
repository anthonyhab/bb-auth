#include "../src/fallback/FallbackClient.hpp"
#include "../src/fallback/FallbackWindow.hpp"

#include <QtTest/QtTest>

namespace bb {

    class FallbackWindowTouchModelTest : public QObject {
        Q_OBJECT

      private slots:
        void fingerprintInfoClassifiesAsTouchAuth();
        void securityKeyInfoClassifiesAsTouchAuth();
        void plainPolkitPromptRequiresPassword();
        void pinentryPromptRemainsPassphraseDriven();

      private:
        static QJsonObject makeEvent(const QString& source, const QString& message, const QString& info = QString());
    };

    QJsonObject FallbackWindowTouchModelTest::makeEvent(const QString& source, const QString& message, const QString& info) {
        QJsonObject context{{"message", message}, {"requestor", QJsonObject{{"name", "test-app"}}}};
        QJsonObject event{{"type", "session.created"}, {"id", "session-1"}, {"source", source}, {"context", context}};
        if (!info.isEmpty()) {
            event.insert("info", info);
        }
        return event;
    }

    void FallbackWindowTouchModelTest::fingerprintInfoClassifiesAsTouchAuth() {
        FallbackClient    client("/tmp/bb-auth-test.sock");
        FallbackWindow    window(&client);

        const QJsonObject event = makeEvent("polkit", "Authentication is required", "Swipe your fingerprint sensor");
        const auto        model = window.buildDisplayModel(event);

        QCOMPARE(model.intent, FallbackWindow::PromptIntent::Fingerprint);
        QVERIFY(model.allowEmptyResponse);
        QCOMPARE(model.prompt, QString("Press Enter to continue (or wait)"));
    }

    void FallbackWindowTouchModelTest::securityKeyInfoClassifiesAsTouchAuth() {
        FallbackClient    client("/tmp/bb-auth-test.sock");
        FallbackWindow    window(&client);

        const QJsonObject event = makeEvent("polkit", "Authentication is required", "Touch your security key to continue");
        const auto        model = window.buildDisplayModel(event);

        QCOMPARE(model.intent, FallbackWindow::PromptIntent::Fido2);
        QVERIFY(model.allowEmptyResponse);
        QCOMPARE(model.prompt, QString("Press Enter to continue (or wait)"));
    }

    void FallbackWindowTouchModelTest::plainPolkitPromptRequiresPassword() {
        FallbackClient    client("/tmp/bb-auth-test.sock");
        FallbackWindow    window(&client);

        const QJsonObject event = makeEvent("polkit", "Authentication is required to install software");
        const auto        model = window.buildDisplayModel(event);

        QCOMPARE(model.prompt, QString("Password:"));
        QVERIFY(!model.allowEmptyResponse);
    }

    void FallbackWindowTouchModelTest::pinentryPromptRemainsPassphraseDriven() {
        FallbackClient    client("/tmp/bb-auth-test.sock");
        FallbackWindow    window(&client);

        const QJsonObject context{{"message", ""}, {"description", "Unlock OpenPGP secret key"}, {"requestor", QJsonObject{{"name", "gpg"}}}};
        const QJsonObject event{{"type", "session.created"}, {"id", "session-2"}, {"source", "pinentry"}, {"context", context}};

        const auto        model = window.buildDisplayModel(event);

        QCOMPARE(model.prompt, QString("Passphrase:"));
        QVERIFY(!model.allowEmptyResponse);
    }

} // namespace bb

int runFallbackWindowTouchModelTests(int argc, char** argv) {
    bb::FallbackWindowTouchModelTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_fallback_touch_model.moc"
