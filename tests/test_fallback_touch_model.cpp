#include "../src/fallback/prompt/PromptModelBuilder.hpp"
#include "../src/fallback/prompt/PromptModel.hpp"

#include <QtTest/QtTest>

namespace bb {

    class PromptModelBuilderTouchTest : public QObject {
        Q_OBJECT

      private slots:
        void fingerprintInfoClassifiesAsTouchAuth();
        void securityKeyInfoClassifiesAsTouchAuth();
        void promptFieldTouchHintClassifiesAsTouchAuth();
        void plainPolkitPromptRequiresPassword();
        void polkitActionIdImprovesActionClarity();
        void polkitUnknownRequestorFallsBackToPid();
        void keyringUnlockUsesStandardizedCopy();
        void pinentryPromptRemainsPassphraseDriven();
        void pinentryPromptUpdateOverridesContextMessage();

      private:
        static QJsonObject makeEvent(const QString& source, const QString& message, const QString& info = QString());
    };

    QJsonObject PromptModelBuilderTouchTest::makeEvent(const QString& source, const QString& message, const QString& info) {
        QJsonObject context{{"message", message}, {"requestor", QJsonObject{{"name", "test-app"}}}};
        QJsonObject event{{"type", "session.created"}, {"id", "session-1"}, {"source", source}, {"context", context}};
        if (!info.isEmpty()) {
            event.insert("info", info);
        }
        return event;
    }

    void PromptModelBuilderTouchTest::fingerprintInfoClassifiesAsTouchAuth() {
        const fallback::prompt::PromptModelBuilder builder;
        const QJsonObject                          event = makeEvent("polkit", "Authentication is required", "Swipe your fingerprint sensor");
        const auto                                 model = builder.build(event);

        QCOMPARE(model.intent, fallback::prompt::PromptIntent::Fingerprint);
        QVERIFY(model.allowEmptyResponse);
        QCOMPARE(model.prompt, QString("Press Enter to continue (or wait)"));
    }

    void PromptModelBuilderTouchTest::securityKeyInfoClassifiesAsTouchAuth() {
        const fallback::prompt::PromptModelBuilder builder;
        const QJsonObject                          event = makeEvent("polkit", "Authentication is required", "Touch your security key to continue");
        const auto                                 model = builder.build(event);

        QCOMPARE(model.intent, fallback::prompt::PromptIntent::Fido2);
        QVERIFY(model.allowEmptyResponse);
        QCOMPARE(model.prompt, QString("Press Enter to continue (or wait)"));
    }

    void PromptModelBuilderTouchTest::plainPolkitPromptRequiresPassword() {
        const fallback::prompt::PromptModelBuilder builder;
        const QJsonObject                          event = makeEvent("polkit", "Authentication is required to install software");
        const auto                                 model = builder.build(event);

        QCOMPARE(model.title, QString("Authorization Required"));
        QCOMPARE(model.prompt, QString("Password:"));
        QVERIFY(!model.allowEmptyResponse);
    }

    void PromptModelBuilderTouchTest::promptFieldTouchHintClassifiesAsTouchAuth() {
        const fallback::prompt::PromptModelBuilder builder;
        QJsonObject                                event = makeEvent("polkit", "Authentication is required");
        event.insert("prompt", "Touch your security key to continue");

        const auto model = builder.build(event);

        QCOMPARE(model.intent, fallback::prompt::PromptIntent::Fido2);
        QVERIFY(model.allowEmptyResponse);
        QCOMPARE(model.prompt, QString("Press Enter to continue (or wait)"));
    }

    void PromptModelBuilderTouchTest::keyringUnlockUsesStandardizedCopy() {
        const fallback::prompt::PromptModelBuilder builder;
        const QJsonObject context{
            {"message", "Authenticate to unlock Login keyring"},
            {"keyringName", "Unlock Login keyring"},
            {"requestor", QJsonObject{{"name", "gnome-keyring"}}},
        };
        const QJsonObject event{{"type", "session.created"}, {"id", "session-3"}, {"source", "keyring"}, {"context", context}};

        const auto model = builder.build(event);

        QCOMPARE(model.intent, fallback::prompt::PromptIntent::Unlock);
        QCOMPARE(model.title, QString("Unlock Login keyring"));
        QCOMPARE(model.summary, QString("Use your password to unlock Login keyring"));
        QCOMPARE(model.prompt, QString("Password:"));
        QVERIFY(!model.allowEmptyResponse);
    }

    void PromptModelBuilderTouchTest::polkitActionIdImprovesActionClarity() {
        const fallback::prompt::PromptModelBuilder builder;
        const QJsonObject context{
            {"message", "Authentication is required"},
            {"actionId", "org.freedesktop.packagekit.system-update"},
            {"user", "root"},
            {"requestor", QJsonObject{{"name", "Software Center"}, {"pid", 4242}}},
        };
        const QJsonObject event{{"type", "session.created"}, {"id", "session-4"}, {"source", "polkit"}, {"context", context}};

        const auto model = builder.build(event);

        QCOMPARE(model.title, QString("Authorization Required"));
        QCOMPARE(model.summary, QString("System Update"));
        QCOMPARE(model.requestor, QString("Requested by Software Center"));
        QVERIFY(model.details.contains("Action: System Update"));
        QVERIFY(model.details.contains("Policy: org.freedesktop.packagekit.system-update"));
        QVERIFY(model.details.contains("Authenticate as root"));
    }

    void PromptModelBuilderTouchTest::polkitUnknownRequestorFallsBackToPid() {
        const fallback::prompt::PromptModelBuilder builder;
        const QJsonObject context{
            {"message", "Authentication is required"},
            {"requestor", QJsonObject{{"name", "Unknown"}, {"pid", 1099}}},
        };
        const QJsonObject event{{"type", "session.created"}, {"id", "session-5"}, {"source", "polkit"}, {"context", context}};

        const auto model = builder.build(event);

        QCOMPARE(model.requestor, QString("Requested by process 1099"));
    }

    void PromptModelBuilderTouchTest::pinentryPromptRemainsPassphraseDriven() {
        const fallback::prompt::PromptModelBuilder builder;

        const QJsonObject                          context{{"message", ""}, {"description", "Unlock OpenPGP secret key"}, {"requestor", QJsonObject{{"name", "gpg"}}}};
        const QJsonObject                          event{{"type", "session.created"}, {"id", "session-2"}, {"source", "pinentry"}, {"context", context}};

        const auto                                 model = builder.build(event);

        QCOMPARE(model.title, QString("Unlock OpenPGP Key"));
        QCOMPARE(model.prompt, QString("Passphrase:"));
        QVERIFY(!model.allowEmptyResponse);
    }

    void PromptModelBuilderTouchTest::pinentryPromptUpdateOverridesContextMessage() {
        const fallback::prompt::PromptModelBuilder builder;

        const QJsonObject context{{"message", "Passphrase:"}, {"description", "Unlock OpenPGP secret key"}, {"requestor", QJsonObject{{"name", "gpg"}}}};
        QJsonObject       event{{"type", "session.updated"}, {"id", "session-2"}, {"source", "pinentry"}, {"context", context}};
        event.insert("prompt", "PIN:");

        const auto model = builder.build(event);

        QCOMPARE(model.prompt, QString("PIN:"));
        QVERIFY(!model.allowEmptyResponse);
    }

} // namespace bb

int runFallbackWindowTouchModelTests(int argc, char** argv) {
    bb::PromptModelBuilderTouchTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_fallback_touch_model.moc"
