#include "../src/core/RequestContext.hpp"
#include <QtTest/QtTest>
#include <QJsonObject>
#include <QString>

class ClassifyRequestTest : public QObject {
    Q_OBJECT

  private slots:
    void testPolkitSource();
    void testGpgKeyring();
    void testSshKeyring();
    void testGenericKeyring();
    void testUnknownSource();
    void testNormalization();
};

void ClassifyRequestTest::testPolkitSource() {
    QJsonObject result = RequestContextHelper::classifyRequest("polkit", "Some Title", "Some Desc");
    QCOMPARE(result.value("kind").toString(), QString("polkit"));
    QCOMPARE(result.value("iconName").toString(), QString("security-high"));
    QCOMPARE(result.value("colorize").toBool(), true);
}

void ClassifyRequestTest::testGpgKeyring() {
    // Case insensitive "gpg" in title
    QJsonObject result1 = RequestContextHelper::classifyRequest("keyring", "Unlock GPG key", "desc");
    QCOMPARE(result1.value("kind").toString(), QString("gpg"));
    QCOMPARE(result1.value("iconName").toString(), QString("gnupg"));
    QCOMPARE(result1.value("colorize").toBool(), true);

    // "OpenPGP" in description
    QJsonObject result2 = RequestContextHelper::classifyRequest("keyring", "Unlock Key", "This is an OpenPGP key");
    QCOMPARE(result2.value("kind").toString(), QString("gpg"));
    QCOMPARE(result2.value("iconName").toString(), QString("gnupg"));
}

void ClassifyRequestTest::testSshKeyring() {
    // "ssh" in title
    QJsonObject result1 = RequestContextHelper::classifyRequest("keyring", "Unlock SSH key", "desc");
    QCOMPARE(result1.value("kind").toString(), QString("ssh"));
    QCOMPARE(result1.value("iconName").toString(), QString("ssh-key"));
    QCOMPARE(result1.value("colorize").toBool(), true);

    // "ssh" in description
    QJsonObject result2 = RequestContextHelper::classifyRequest("keyring", "Unlock Key", "Enter passphrase for SSH key");
    QCOMPARE(result2.value("kind").toString(), QString("ssh"));
    QCOMPARE(result2.value("iconName").toString(), QString("ssh-key"));
}

void ClassifyRequestTest::testGenericKeyring() {
    QJsonObject result = RequestContextHelper::classifyRequest("keyring", "Unlock Default Keyring", "Please enter password");
    QCOMPARE(result.value("kind").toString(), QString("keyring"));
    QVERIFY(!result.contains("iconName")); // icon is empty for generic keyring
    QCOMPARE(result.value("colorize").toBool(), true);
}

void ClassifyRequestTest::testUnknownSource() {
    QJsonObject result = RequestContextHelper::classifyRequest("unknown", "Title", "Desc");
    QCOMPARE(result.value("kind").toString(), QString("unknown"));
    QVERIFY(!result.contains("iconName"));
    QCOMPARE(result.value("colorize").toBool(), false);
}

void ClassifyRequestTest::testNormalization() {
    // Test helper normalizePrompt as well since it's related logic often used with classify
    QString s1 = "Password: ";
    QCOMPARE(RequestContextHelper::normalizePrompt(s1), QString("Password"));

    QString s2 = "Passphrase："; // Japanese/Fullwidth colon
    QCOMPARE(RequestContextHelper::normalizePrompt(s2), QString("Passphrase"));

    QString s3 = "  Pin  ";
    QCOMPARE(RequestContextHelper::normalizePrompt(s3), QString("Pin"));
}

int runClassifyRequestTests(int argc, char** argv) {
    ClassifyRequestTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_classify_request.moc"
