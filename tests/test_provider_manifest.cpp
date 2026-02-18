#include "../src/core/providers/ProviderManifest.hpp"

#include <QtTest/QtTest>

namespace bb {

    class ProviderManifestTest : public QObject {
        Q_OBJECT

      private slots:
        void parseValidManifest();
        void parseValidManifestWithDefaults();
        void rejectInvalidJson();
        void rejectMissingRequiredFields();
        void rejectInvalidId();
        void rejectOutOfRangePriority();
        void rejectInvalidExecPath();
        void rejectInvalidArgs();
        void rejectInvalidEnv();
    };

    void ProviderManifestTest::parseValidManifest() {
        const QByteArray json = R"({
        "id":"gtk-fallback",
        "name":"GTK Fallback",
        "kind":"gtk-fallback",
        "priority":15,
        "exec":"/usr/libexec/bb-auth-gtk-fallback",
        "args":["--socket","/tmp/socket"],
        "env":{"GTK_THEME":"Adwaita:dark"},
        "autostart":false,
        "capabilities":["password","status"]
    })";

        const auto       result = providers::parseProviderManifest(json, "/tmp/gtk-fallback.json");
        QVERIFY(result.ok);
        QCOMPARE(result.manifest.id, QString("gtk-fallback"));
        QCOMPARE(result.manifest.name, QString("GTK Fallback"));
        QCOMPARE(result.manifest.kind, QString("gtk-fallback"));
        QCOMPARE(result.manifest.priority, 15);
        QCOMPARE(result.manifest.exec, QString("/usr/libexec/bb-auth-gtk-fallback"));
        QCOMPARE(result.manifest.args, QStringList({"--socket", "/tmp/socket"}));
        QCOMPARE(result.manifest.env.value("GTK_THEME").toString(), QString("Adwaita:dark"));
        QCOMPARE(result.manifest.autostart, false);
        QCOMPARE(result.manifest.capabilities, QStringList({"password", "status"}));
        QCOMPARE(result.manifest.sourcePath, QString("/tmp/gtk-fallback.json"));
        QVERIFY(result.manifest.isValid());
    }

    void ProviderManifestTest::parseValidManifestWithDefaults() {
        const QByteArray json = R"({
        "id":"qt-fallback",
        "name":"Qt Fallback",
        "kind":"fallback",
        "exec":"bb-auth-fallback"
    })";

        const auto       result = providers::parseProviderManifest(json);
        QVERIFY(result.ok);
        QCOMPARE(result.manifest.priority, 0);
        QCOMPARE(result.manifest.autostart, true);
        QVERIFY(result.manifest.args.isEmpty());
        QVERIFY(result.manifest.env.isEmpty());
        QVERIFY(result.manifest.capabilities.isEmpty());
    }

    void ProviderManifestTest::rejectInvalidJson() {
        const auto result = providers::parseProviderManifest("not-json");
        QVERIFY(!result.ok);
        QVERIFY(result.error.contains("invalid JSON"));
    }

    void ProviderManifestTest::rejectMissingRequiredFields() {
        const QByteArray json = R"({
        "id":"missing-exec",
        "name":"Missing Exec",
        "kind":"fallback"
    })";

        const auto       result = providers::parseProviderManifest(json);
        QVERIFY(!result.ok);
        QCOMPARE(result.error, QString("exec is required"));
    }

    void ProviderManifestTest::rejectInvalidId() {
        const QByteArray json = R"({
        "id":"INVALID ID",
        "name":"Bad",
        "kind":"fallback",
        "exec":"bb-auth-fallback"
    })";

        const auto       result = providers::parseProviderManifest(json);
        QVERIFY(!result.ok);
        QVERIFY(result.error.contains("id must match"));
    }

    void ProviderManifestTest::rejectOutOfRangePriority() {
        const QByteArray json = R"({
        "id":"bad-priority",
        "name":"Bad",
        "kind":"fallback",
        "priority":1001,
        "exec":"bb-auth-fallback"
    })";

        const auto       result = providers::parseProviderManifest(json);
        QVERIFY(!result.ok);
        QCOMPARE(result.error, QString("priority must be within [-1000, 1000]"));
    }

    void ProviderManifestTest::rejectInvalidExecPath() {
        const QByteArray json = R"({
        "id":"bad-exec",
        "name":"Bad",
        "kind":"fallback",
        "exec":"relative/path/provider"
    })";

        const auto       result = providers::parseProviderManifest(json);
        QVERIFY(!result.ok);
        QCOMPARE(result.error, QString("exec must be absolute path or basename"));
    }

    void ProviderManifestTest::rejectInvalidArgs() {
        const QByteArray json = R"({
        "id":"bad-args",
        "name":"Bad",
        "kind":"fallback",
        "exec":"bb-auth-fallback",
        "args":["ok", 1]
    })";

        const auto       result = providers::parseProviderManifest(json);
        QVERIFY(!result.ok);
        QCOMPARE(result.error, QString("args must contain only strings"));
    }

    void ProviderManifestTest::rejectInvalidEnv() {
        const QByteArray json = R"({
        "id":"bad-env",
        "name":"Bad",
        "kind":"fallback",
        "exec":"bb-auth-fallback",
        "env":{"A":1}
    })";

        const auto       result = providers::parseProviderManifest(json);
        QVERIFY(!result.ok);
        QCOMPARE(result.error, QString("env values must be strings"));
    }

} // namespace bb

int runProviderManifestTests(int argc, char** argv) {
    bb::ProviderManifestTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_provider_manifest.moc"
