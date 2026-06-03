#include "../src/core/providers/ProviderDiscovery.hpp"

#include <QtTest/QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace bb {

    class ProviderDiscoveryTest : public QObject {
        Q_OBJECT

      private slots:
        void discoverRespectsDirectoryPrecedenceAndDedupesById();
        void discoverUsesLexicalOrderWithinDirectory();
        void discoverSkipsInvalidManifestsWithWarnings();
        void discoverSkipsOversizedManifestWithoutReadingIt();
    };

    namespace {

        void writeFile(const QString& path, const QByteArray& content) {
            QFile file(path);
            QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text), qPrintable(path));
            file.write(content);
            file.close();
        }

    } // namespace

    void ProviderDiscoveryTest::discoverRespectsDirectoryPrecedenceAndDedupesById() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());

        const QString highDir = temp.path() + "/high";
        const QString lowDir  = temp.path() + "/low";
        QVERIFY(QDir().mkpath(highDir));
        QVERIFY(QDir().mkpath(lowDir));

        writeFile(highDir + "/gtk.json", R"({"id":"gtk","name":"GTK High","kind":"gtk","priority":20,"exec":"/bin/true"})");
        writeFile(lowDir + "/gtk.json", R"({"id":"gtk","name":"GTK Low","kind":"gtk","priority":5,"exec":"/bin/false"})");
        writeFile(lowDir + "/qt.json", R"({"id":"qt","name":"Qt","kind":"fallback","priority":10,"exec":"/bin/true"})");

        const auto result = providers::ProviderDiscovery::discover(QStringList{highDir, lowDir});

        QCOMPARE(result.manifests.size(), 2);
        QCOMPARE(result.manifests[0].id, QString("gtk"));
        QCOMPARE(result.manifests[0].name, QString("GTK High"));
        QCOMPARE(result.manifests[1].id, QString("qt"));
        QVERIFY(std::any_of(result.warnings.begin(), result.warnings.end(), [](const QString& warning) { return warning.contains("duplicate id 'gtk'"); }));
    }

    void ProviderDiscoveryTest::discoverUsesLexicalOrderWithinDirectory() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());

        const QString dir = temp.path() + "/providers";
        QVERIFY(QDir().mkpath(dir));

        writeFile(dir + "/20-b.json", R"({"id":"b","name":"B","kind":"fallback","priority":1,"exec":"/bin/true"})");
        writeFile(dir + "/10-a.json", R"({"id":"a","name":"A","kind":"fallback","priority":1,"exec":"/bin/true"})");

        const auto result = providers::ProviderDiscovery::discover(QStringList{dir});
        QCOMPARE(result.manifests.size(), 2);
        QCOMPARE(result.manifests[0].id, QString("a"));
        QCOMPARE(result.manifests[1].id, QString("b"));
    }

    void ProviderDiscoveryTest::discoverSkipsInvalidManifestsWithWarnings() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());

        const QString dir = temp.path() + "/providers";
        QVERIFY(QDir().mkpath(dir));

        writeFile(dir + "/bad.json", R"({"id":"bad","name":"Bad","kind":"fallback"})");
        writeFile(dir + "/good.json", R"({"id":"good","name":"Good","kind":"fallback","priority":0,"exec":"/bin/true"})");

        const auto result = providers::ProviderDiscovery::discover(QStringList{dir});
        QCOMPARE(result.manifests.size(), 1);
        QCOMPARE(result.manifests[0].id, QString("good"));
        QVERIFY(std::any_of(result.warnings.begin(), result.warnings.end(),
                            [](const QString& warning) { return warning.contains("Skipping manifest") && warning.contains("exec is required"); }));
    }

    void ProviderDiscoveryTest::discoverSkipsOversizedManifestWithoutReadingIt() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());

        const QString dir = temp.path() + "/providers";
        QVERIFY(QDir().mkpath(dir));

        // A manifest larger than the 64 KiB cap must be skipped before being read into
        // memory (F6) — even if its content would otherwise parse.
        QByteArray oversized = R"({"id":"huge","name":"Huge","kind":"fallback","priority":0,"exec":"/bin/true","pad":")";
        oversized.append(QByteArray(70 * 1024, 'x'));
        oversized.append("\"}");
        writeFile(dir + "/huge.json", oversized);
        writeFile(dir + "/good.json", R"({"id":"good","name":"Good","kind":"fallback","priority":0,"exec":"/bin/true"})");

        const auto result = providers::ProviderDiscovery::discover(QStringList{dir});
        QCOMPARE(result.manifests.size(), 1);
        QCOMPARE(result.manifests[0].id, QString("good"));
        QVERIFY(std::any_of(result.warnings.begin(), result.warnings.end(),
                            [](const QString& warning) { return warning.contains("huge.json") && warning.contains("exceeds maximum size"); }));
    }

} // namespace bb

int runProviderDiscoveryTests(int argc, char** argv) {
    bb::ProviderDiscoveryTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_provider_discovery.moc"
