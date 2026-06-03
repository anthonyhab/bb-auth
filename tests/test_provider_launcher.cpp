#include "../src/core/providers/ProviderLauncher.hpp"

#include <QtTest/QtTest>

namespace bb {

    class ProviderLauncherTest : public QObject {
        Q_OBJECT

      private slots:
        void usesLegacyEnvOverrideWhenSet();
        void choosesHighestPriorityAutostartProvider();
        void fallsBackToLegacyBinaryWhenNoManifestCandidate();
        void appliesBackoffAfterFailedLaunch();
        void skipsLaunchWhenActiveProviderOrNoSessions();
        void eagerLaunchesAutostartProviderWithNoPendingSessions();
        void eagerNeverFallsBackToLegacyBinary();
    };

    void ProviderLauncherTest::usesLegacyEnvOverrideWhenSet() {
        qint64                             nowMs = 1000;
        QString                            launchedProgram;
        QStringList                        launchedArgs;
        QProcessEnvironment                launchedEnv;

        providers::ProviderLauncher        launcher([&nowMs] { return nowMs; },
                                             [&launchedProgram, &launchedArgs, &launchedEnv](const QString& program, const QStringList& args, const QProcessEnvironment& env) {
                                                 launchedProgram = program;
                                                 launchedArgs    = args;
                                                 launchedEnv     = env;
                                                 return 4242; // launched pid
                                             });

        QList<providers::ProviderManifest> manifests;
        providers::ProviderManifest        manifest;
        manifest.id        = "gtk";
        manifest.name      = "GTK";
        manifest.kind      = "gtk-fallback";
        manifest.priority  = 50;
        manifest.exec      = "/bin/false";
        manifest.autostart = true;
        manifests.push_back(manifest);

        const auto result = launcher.tryLaunch(manifests, "/tmp/bb-auth.sock", "session-created", false, true, "/bin/true", "/bin/false");

        QVERIFY(result.attempted);
        QVERIFY(result.launched);
        QCOMPARE(result.providerId, QString("__legacy_env__"));
        QCOMPARE(launchedProgram, QString("/bin/true"));
        QCOMPARE(launchedArgs, QStringList({"--socket", "/tmp/bb-auth.sock"}));
        QVERIFY(launchedEnv.contains("PATH"));
    }

    void ProviderLauncherTest::choosesHighestPriorityAutostartProvider() {
        qint64                             nowMs = 1000;
        QString                            launchedProgram;

        providers::ProviderLauncher        launcher([&nowMs] { return nowMs; },
                                             [&launchedProgram](const QString& program, const QStringList&, const QProcessEnvironment&) {
                                                 launchedProgram = program;
                                                 return 4242; // launched pid
                                             });

        QList<providers::ProviderManifest> manifests;

        providers::ProviderManifest        low;
        low.id        = "low";
        low.name      = "Low";
        low.kind      = "fallback";
        low.priority  = 10;
        low.exec      = "/bin/true";
        low.autostart = true;

        providers::ProviderManifest high;
        high.id        = "high";
        high.name      = "High";
        high.kind      = "gtk-fallback";
        high.priority  = 20;
        high.exec      = "/not/existing/high-provider";
        high.autostart = true;

        providers::ProviderManifest disabled;
        disabled.id        = "disabled";
        disabled.name      = "Disabled";
        disabled.kind      = "gtk-fallback";
        disabled.priority  = 100;
        disabled.exec      = "/bin/true";
        disabled.autostart = false;

        manifests << low << high << disabled;

        const auto result = launcher.tryLaunch(manifests, "/tmp/bb-auth.sock", "provider-prune", false, true, QString(), "/bin/false");

        QVERIFY(result.attempted);
        QVERIFY(result.launched);
        QCOMPARE(result.providerId, QString("low"));
        QCOMPARE(launchedProgram, QString("/bin/true"));
    }

    void ProviderLauncherTest::fallsBackToLegacyBinaryWhenNoManifestCandidate() {
        qint64                             nowMs = 1000;

        providers::ProviderLauncher        launcher([&nowMs] { return nowMs; }, [](const QString&, const QStringList&, const QProcessEnvironment&) { return qint64{4242}; });

        QList<providers::ProviderManifest> manifests;

        const auto                         result = launcher.tryLaunch(manifests, "/tmp/bb-auth.sock", "session-created", false, true, QString(), "/bin/true");

        QVERIFY(result.attempted);
        QVERIFY(result.launched);
        QCOMPARE(result.providerId, QString("__legacy_default__"));
    }

    void ProviderLauncherTest::appliesBackoffAfterFailedLaunch() {
        qint64                             nowMs    = 1000;
        int                                attempts = 0;

        providers::ProviderLauncher        launcher([&nowMs] { return nowMs; },
                                             [&attempts](const QString&, const QStringList&, const QProcessEnvironment&) {
                                                 attempts += 1;
                                                 return qint64{0}; // launch failed
                                             });

        QList<providers::ProviderManifest> manifests;
        providers::ProviderManifest        manifest;
        manifest.id        = "gtk";
        manifest.name      = "GTK";
        manifest.kind      = "gtk-fallback";
        manifest.priority  = 10;
        manifest.exec      = "/bin/true";
        manifest.autostart = true;
        manifests.push_back(manifest);

        const auto first = launcher.tryLaunch(manifests, "/tmp/bb-auth.sock", "session-created", false, true, QString(), "/bin/false");
        QVERIFY(first.attempted);
        QVERIFY(!first.launched);
        QCOMPARE(attempts, 1);

        const auto second = launcher.tryLaunch(manifests, "/tmp/bb-auth.sock", "session-created", false, true, QString(), "/bin/false");
        QVERIFY(!second.attempted);
        QVERIFY(second.detail.contains("throttled"));
        QCOMPARE(attempts, 1);
    }

    void ProviderLauncherTest::skipsLaunchWhenActiveProviderOrNoSessions() {
        qint64                             nowMs    = 1000;
        int                                attempts = 0;

        providers::ProviderLauncher        launcher([&nowMs] { return nowMs; },
                                             [&attempts](const QString&, const QStringList&, const QProcessEnvironment&) {
                                                 attempts += 1;
                                                 return qint64{4242};
                                             });

        QList<providers::ProviderManifest> manifests;

        const auto                         withActive = launcher.tryLaunch(manifests, "/tmp/bb-auth.sock", "session-created", true, true, QString(), "/bin/true");
        QVERIFY(!withActive.attempted);

        const auto noSessions = launcher.tryLaunch(manifests, "/tmp/bb-auth.sock", "session-created", false, false, QString(), "/bin/true");
        QVERIFY(!noSessions.attempted);

        QCOMPARE(attempts, 0);
    }

    void ProviderLauncherTest::eagerLaunchesAutostartProviderWithNoPendingSessions() {
        qint64                             nowMs = 1000;
        QString                            launchedProgram;

        providers::ProviderLauncher        launcher([&nowMs] { return nowMs; },
                                             [&launchedProgram](const QString& program, const QStringList&, const QProcessEnvironment&) {
                                                 launchedProgram = program;
                                                 return qint64{4242};
                                             });

        QList<providers::ProviderManifest> manifests;
        providers::ProviderManifest        manifest;
        manifest.id        = "omarchy-prompt";
        manifest.name      = "Omarchy";
        manifest.kind      = "quickshell";
        manifest.priority  = 100;
        manifest.exec      = "/bin/true";
        manifest.autostart = true;
        manifests.push_back(manifest);

        // No active provider, NO pending sessions — on-demand would skip, but eager launches.
        const auto result = launcher.tryLaunch(manifests, "/tmp/bb-auth.sock", "daemon-startup", false, false, QString(), "/bin/false", /*eager=*/true);

        QVERIFY(result.attempted);
        QVERIFY(result.launched);
        QCOMPARE(result.providerId, QString("omarchy-prompt"));
        QCOMPARE(launchedProgram, QString("/bin/true"));
    }

    void ProviderLauncherTest::eagerNeverFallsBackToLegacyBinary() {
        qint64                             nowMs    = 1000;
        int                                attempts = 0;

        providers::ProviderLauncher        launcher([&nowMs] { return nowMs; },
                                             [&attempts](const QString&, const QStringList&, const QProcessEnvironment&) {
                                                 attempts += 1;
                                                 return qint64{4242};
                                             });

        QList<providers::ProviderManifest> manifests; // none configured

        // Eager must never resurrect the legacy env override or the built-in fallback —
        // those stay on-demand safety nets. With no autostart provider it is a no-op.
        const auto withLegacyEnv = launcher.tryLaunch(manifests, "/tmp/bb-auth.sock", "daemon-startup", false, false, "/bin/true", "/bin/true", /*eager=*/true);
        QVERIFY(!withLegacyEnv.attempted);
        QVERIFY(withLegacyEnv.detail.contains("eager"));

        QCOMPARE(attempts, 0);
    }

} // namespace bb

int runProviderLauncherTests(int argc, char** argv) {
    bb::ProviderLauncherTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_provider_launcher.moc"
