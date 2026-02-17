#include "../src/fallback/prompt/PromptExtractors.hpp"

#include <QtTest/QtTest>

using namespace bb::fallback::prompt;

class PromptExtractorsTest : public QObject {
    Q_OBJECT

  private slots:
    void extractCommandName_explicitRun();
    void extractCommandName_path();
    void extractCommandName_simple();
    void extractCommandName_empty();

    void extractUnlockTarget_simple();
    void extractUnlockTarget_trailingPeriod();
    void extractUnlockTarget_normalization();
    void extractUnlockTarget_empty();

    void extractUnlockTargetFromContext_priority();
    void extractUnlockTargetFromContext_fallback();

    void buildUnlockDetails_combinesUnique();
    void buildUnlockDetails_filtersTemplateLines();
};

void PromptExtractorsTest::extractCommandName_explicitRun() {
    QCOMPARE(extractCommandName("run 'my_command'"), "my_command");
    QCOMPARE(extractCommandName("run \"my_command\""), "my_command");
    QCOMPARE(extractCommandName("Please run `sudo` to continue"), "sudo");
}

void PromptExtractorsTest::extractCommandName_path() {
    QCOMPARE(extractCommandName("/usr/bin/git"), "git");
    QCOMPARE(extractCommandName("The process /opt/custom/app is requesting access"), "app");
}

void PromptExtractorsTest::extractCommandName_simple() {
    // extractCommandName looks for "run <cmd>" or an absolute path.
    // A simple command name without path or "run" prefix should not be captured.
    QCOMPARE(extractCommandName("simple_command"), "");
}

void PromptExtractorsTest::extractCommandName_empty() {
    QCOMPARE(extractCommandName(""), "");
    QCOMPARE(extractCommandName("   "), "");
}

void PromptExtractorsTest::extractUnlockTarget_simple() {
    QCOMPARE(extractUnlockTarget("unlock my_secret_key"), "my_secret_key");
    QCOMPARE(extractUnlockTarget("Unlock Default Keyring"), "Default Keyring");
}

void PromptExtractorsTest::extractUnlockTarget_trailingPeriod() {
    QCOMPARE(extractUnlockTarget("unlock login."), "login");
}

void PromptExtractorsTest::extractUnlockTarget_normalization() {
    // The implementation uses normalizeDetailText which lowercases and trims.
    // And extractUnlockTarget looks for "unlock <capture>".
    QCOMPARE(extractUnlockTarget("  UNLOCK   My_Target  "), "My_Target");
}

void PromptExtractorsTest::extractUnlockTarget_empty() {
    QCOMPARE(extractUnlockTarget(""), "");
    QCOMPARE(extractUnlockTarget("just some text"), "");
}

void PromptExtractorsTest::extractUnlockTargetFromContext_priority() {
    QJsonObject context;
    context.insert("keyringName", "unlock KeyringA");
    context.insert("message", "unlock KeyringB");
    context.insert("description", "unlock KeyringC");

    QCOMPARE(extractUnlockTargetFromContext(context), "KeyringA");
}

void PromptExtractorsTest::extractUnlockTargetFromContext_fallback() {
    QJsonObject context1;
    context1.insert("message", "unlock KeyringB");
    context1.insert("description", "unlock KeyringC");
    QCOMPARE(extractUnlockTargetFromContext(context1), "KeyringB");

    QJsonObject context2;
    context2.insert("description", "unlock KeyringC");
    QCOMPARE(extractUnlockTargetFromContext(context2), "KeyringC");
}

void PromptExtractorsTest::buildUnlockDetails_combinesUnique() {
    QJsonObject context;
    context.insert("description", "Line 1\nLine 2");
    context.insert("message", "Line 2\nLine 3");
    context.insert("keyringName", "Line 4");

    QString result = buildUnlockDetails(context, "Target");

    QVERIFY(result.contains("Line 1"));
    QVERIFY(result.contains("Line 2"));
    QVERIFY(result.contains("Line 3"));
    QVERIFY(result.contains("Line 4"));

    // Check for uniqueness
    QCOMPARE(result.count("Line 2"), 1);
}

void PromptExtractorsTest::buildUnlockDetails_filtersTemplateLines() {
    QJsonObject context;
    context.insert("description", "Authenticate to unlock Login\nSome detail");

    // "Authenticate to unlock Login" should be filtered out if target is "Login"
    QString result = buildUnlockDetails(context, "Login");

    QVERIFY(!result.contains("Authenticate to unlock Login"));
    QVERIFY(result.contains("Some detail"));
}

int runPromptExtractorsTests(int argc, char** argv) {
    PromptExtractorsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_prompt_extractors.moc"
