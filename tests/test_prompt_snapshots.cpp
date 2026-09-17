// Golden-snapshot regression tests: the complete PromptDisplayModel surface for
// each canonical auth scenario. Any drift in title/summary/requestor/reason or
// intent classification fails loudly — these are the prompts a user sees.

#include "../src/fallback/prompt/PromptModel.hpp"
#include "../src/fallback/prompt/PromptModelBuilder.hpp"

#include <QtTest/QtTest>

namespace bb {

    namespace {

        QJsonObject makeEvent(const QString& source, const QJsonObject& context, const QString& prompt = QString(), const QString& info = QString()) {
            QJsonObject event{{"type", "session.created"}, {"id", "snap"}, {"source", source}, {"context", context}};
            if (!prompt.isEmpty())
                event.insert("prompt", prompt);
            if (!info.isEmpty())
                event.insert("info", info);
            return event;
        }



    } // namespace

    class PromptSnapshotTest : public QObject {
        Q_OBJECT

      private slots:
        void polkitRunCommand();
        void polkitGenericAuth();
        void polkitAgentDeclaredReason();
        void polkitAgentUndeclared();
        void keyringUnlock();
        void pinentryOpenPgp();
        void polkitFingerprint();
    };

    void PromptSnapshotTest::polkitRunCommand() {
        const fallback::prompt::PromptModelBuilder builder;
        const auto model = builder.build(makeEvent("polkit",
                                                   {{"message", "Authentication is required to run '/usr/bin/pacman -Syu'"},
                                                    {"description", "Authentication is required to run '/usr/bin/pacman -Syu'"},
                                                    {"actionId", "org.freedesktop.policykit.exec"},
                                                    {"requestor", QJsonObject{{"name", "terminal"}, {"pid", 100}}}}));

        QCOMPARE(model.intent, fallback::prompt::PromptIntent::RunCommand);
        QCOMPARE(model.title, QString("Authorization Required"));
        QVERIFY(model.requestor.contains("terminal"));
        QVERIFY(!model.agentRequestor);
        QVERIFY(model.reason.isEmpty());
    }

    void PromptSnapshotTest::polkitGenericAuth() {
        const fallback::prompt::PromptModelBuilder builder;
        const auto model = builder.build(makeEvent("polkit",
                                                   {{"message", "Authentication is required"},
                                                    {"requestor", QJsonObject{{"name", "settings-app"}, {"pid", 200}}}}));

        QCOMPARE(model.intent, fallback::prompt::PromptIntent::Generic);
        QCOMPARE(model.title, QString("Authorization Required"));
        QCOMPARE(model.requestor, QString("Requested by settings-app"));
        QVERIFY(!model.agentRequestor);
        QVERIFY(!model.intentMismatch);
    }

    void PromptSnapshotTest::polkitAgentDeclaredReason() {
        const fallback::prompt::PromptModelBuilder builder;
        const auto model = builder.build(makeEvent("polkit",
                                                   {{"message", "Authentication is required"},
                                                    {"requestor", QJsonObject{{"name", "Claude Code"}, {"isAgent", true}, {"agentKind", "claude-code"}, {"pid", 4242}}},
                                                    {"intent", QJsonObject{{"reason", "remove duplicate .desktop file"}, {"declaredAgent", "claude-code"}, {"channel", "hook"}, {"mismatch", false}}}}));

        QCOMPARE(model.requestor, QString("Requested by Claude Code (AI agent)"));
        QCOMPARE(model.reason, QString("remove duplicate .desktop file"));
        QVERIFY(model.agentRequestor);
        QVERIFY(!model.intentMismatch);
    }

    void PromptSnapshotTest::polkitAgentUndeclared() {
        const fallback::prompt::PromptModelBuilder builder;
        const auto model = builder.build(makeEvent("polkit",
                                                   {{"message", "Authentication is required"},
                                                    {"requestor", QJsonObject{{"name", "Gemini CLI"}, {"isAgent", true}, {"agentKind", "gemini-cli"}, {"pid", 7777}}}}));

        // Snapshot invariant: agent provenance is set even with no declaration —
        // the window renders the explicit undeclared-reason state off this flag.
        QVERIFY(model.agentRequestor);
        QVERIFY(model.reason.isEmpty());
        QCOMPARE(model.requestor, QString("Requested by Gemini CLI (AI agent)"));
    }

    void PromptSnapshotTest::keyringUnlock() {
        const fallback::prompt::PromptModelBuilder builder;
        const auto model = builder.build(makeEvent("keyring",
                                                   {{"message", "Authentication is required"},
                                                    {"requestor", QJsonObject{{"name", "mail-client"}, {"pid", 300}}},
                                                    {"keyring", "login"}}));

        QCOMPARE(model.intent, fallback::prompt::PromptIntent::Unlock);
        QVERIFY(model.title.startsWith("Unlock"));
        QVERIFY(model.summary.contains("password", Qt::CaseInsensitive));
    }

    void PromptSnapshotTest::pinentryOpenPgp() {
        const fallback::prompt::PromptModelBuilder builder;
        const auto model = builder.build(makeEvent("pinentry",
                                                   {{"message", "Passphrase:"},
                                                    {"description", "Unlock OpenPGP secret key \"User <u@x>\" ID ABCDEF12, 4096-bit RSA key, created 2024-01-01"},
                                                    {"requestor", QJsonObject{{"name", "gpg"}}}},
                                                   "PIN:"));

        QCOMPARE(model.intent, fallback::prompt::PromptIntent::OpenPgp);
        QCOMPARE(model.title, QString("Unlock OpenPGP Key"));
        QCOMPARE(model.prompt, QString("PIN:"));
    }

    void PromptSnapshotTest::polkitFingerprint() {
        const fallback::prompt::PromptModelBuilder builder;
        const auto model = builder.build(makeEvent("polkit",
                                                   {{"message", "Authentication is required"},
                                                    {"requestor", QJsonObject{{"name", "login-manager"}, {"pid", 1}}}},
                                                   QString(), "Place your finger on the fingerprint reader"));

        QCOMPARE(model.intent, fallback::prompt::PromptIntent::Fingerprint);
        QCOMPARE(model.title, QString("Verify Fingerprint"));
    }

} // namespace bb

int runPromptSnapshotTests(int argc, char** argv) {
    bb::PromptSnapshotTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_prompt_snapshots.moc"
