#include "../src/core/agent/ProviderRegistry.hpp"
#include "../src/core/providers/ProviderDiscovery.hpp"

#include <QtTest/QtTest>

#include <QDir>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>

#include <algorithm>
#include <memory>
#include <utility>

namespace bb {

    namespace {

#define REQUIRE_LOCAL_SOCKET_LISTENING(fixture)                                                                                                                  \
    do {                                                                                                                                                        \
        if (!(fixture).isListening()) {                                                                                                                         \
            QSKIP(qPrintable(QStringLiteral("Skipping local-socket-dependent test: %1").arg((fixture).errorString())));                                        \
        }                                                                                                                                                       \
    } while (false)

        struct ConnectedSocket {
            std::unique_ptr<QLocalSocket> client;
            std::unique_ptr<QLocalSocket> server;
        };

        class LocalSocketFixture {
          public:
            LocalSocketFixture() {
                m_tempDir = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/bb-auth-conformance-XXXXXX");
                if (!m_tempDir->isValid()) {
                    m_lastError = "failed to create temporary directory for local socket";
                    return;
                }

                m_serverPath = m_tempDir->path() + "/agent.sock";
                QLocalServer::removeServer(m_serverPath);
                m_isListening = m_server.listen(m_serverPath);
                if (!m_isListening) {
                    m_lastError = m_server.errorString();
                }
            }

            bool isListening() const {
                return m_isListening;
            }

            QString errorString() const {
                return m_lastError;
            }

            ConnectedSocket connect() {
                auto client = std::make_unique<QLocalSocket>();
                client->connectToServer(m_serverPath);
                if (!client->waitForConnected(1000)) {
                    m_lastError = client->errorString();
                    return ConnectedSocket{};
                }

                if (!m_server.waitForNewConnection(1000)) {
                    m_lastError = m_server.errorString();
                    return ConnectedSocket{};
                }

                QLocalSocket* serverSocket = m_server.nextPendingConnection();
                if (!serverSocket) {
                    m_lastError = "failed to get pending server socket";
                    return ConnectedSocket{};
                }

                serverSocket->setParent(nullptr);
                return ConnectedSocket{std::move(client), std::unique_ptr<QLocalSocket>(serverSocket)};
            }

          private:
            QLocalServer                   m_server;
            std::unique_ptr<QTemporaryDir> m_tempDir;
            QString                        m_serverPath;
            QString                        m_lastError;
            bool                           m_isListening = false;
        };

        class EnvVarGuard {
          public:
            EnvVarGuard(const char* name, const QByteArray& value) : m_name(name), m_hadValue(qEnvironmentVariableIsSet(name)), m_oldValue(qgetenv(name)) {
                qputenv(name, value);
            }

            ~EnvVarGuard() {
                if (m_hadValue) {
                    qputenv(m_name.constData(), m_oldValue);
                } else {
                    qunsetenv(m_name.constData());
                }
            }

          private:
            QByteArray m_name;
            bool       m_hadValue = false;
            QByteArray m_oldValue;
        };

        void writeFile(const QString& path, const QByteArray& content) {
            QFile file(path);
            QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text), qPrintable(path));
            file.write(content);
            file.close();
        }

    } // namespace

    class ProviderConformanceTest : public QObject {
        Q_OBJECT

      private slots:
        void defaultSearchDirs_respectsContractOrder();
        void providerRegistry_assignsExpectedDefaultsAndPriorities();
        void providerRegistry_enforcesActiveProviderAuthorizationBoundary();
        void discovery_honorsDefaultDirectoryPrecedence();
    };

    void ProviderConformanceTest::defaultSearchDirs_respectsContractOrder() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());

        const QString explicitDir = QDir::cleanPath(temp.path() + "/explicit");
        const QString configHome  = QDir::cleanPath(temp.path() + "/config-home");
        const QString dataHome    = QDir::cleanPath(temp.path() + "/data-home");
        const QString systemDir   = QDir::cleanPath(temp.path() + "/system/providers.d");

        EnvVarGuard   providerDirGuard("BB_AUTH_PROVIDER_DIR", explicitDir.toLocal8Bit());
        EnvVarGuard   configHomeGuard("XDG_CONFIG_HOME", configHome.toLocal8Bit());
        EnvVarGuard   dataHomeGuard("XDG_DATA_HOME", dataHome.toLocal8Bit());

        const auto    dirs = providers::ProviderDiscovery::defaultSearchDirs(systemDir);

        QCOMPARE(dirs.size(), 4);
        QCOMPARE(dirs[0], explicitDir);
        QCOMPARE(dirs[1], QDir::cleanPath(configHome + "/bb-auth/providers.d"));
        QCOMPARE(dirs[2], QDir::cleanPath(dataHome + "/bb-auth/providers.d"));
        QCOMPARE(dirs[3], systemDir);
    }

    void ProviderConformanceTest::providerRegistry_assignsExpectedDefaultsAndPriorities() {
        LocalSocketFixture fixture;
        REQUIRE_LOCAL_SOCKET_LISTENING(fixture);

        qint64                  nowMs = 1000;
        agent::ProviderRegistry registry([&nowMs] { return nowMs; });

        ConnectedSocket         quickshellSocket = fixture.connect();
        QVERIFY(quickshellSocket.server != nullptr);
        const auto quickshellProvider = registry.registerProvider(quickshellSocket.server.get(), QJsonObject{{"kind", "quickshell"}});
        QCOMPARE(quickshellProvider.name, QString("unknown"));
        QCOMPARE(quickshellProvider.kind, QString("quickshell"));
        QCOMPARE(quickshellProvider.priority, 100);

        ConnectedSocket fallbackSocket = fixture.connect();
        QVERIFY(fallbackSocket.server != nullptr);
        const auto fallbackProvider = registry.registerProvider(fallbackSocket.server.get(), QJsonObject{{"kind", "fallback"}});
        QCOMPARE(fallbackProvider.priority, 10);

        ConnectedSocket customSocket = fixture.connect();
        QVERIFY(customSocket.server != nullptr);
        const auto customProvider = registry.registerProvider(customSocket.server.get(), QJsonObject{{"kind", "custom"}});
        QCOMPARE(customProvider.priority, 50);

        ConnectedSocket unnamedSocket = fixture.connect();
        QVERIFY(unnamedSocket.server != nullptr);
        const auto unnamedProvider = registry.registerProvider(unnamedSocket.server.get(), QJsonObject{});
        QCOMPARE(unnamedProvider.name, QString("unknown"));
        QCOMPARE(unnamedProvider.kind, QString("unknown"));
        QCOMPARE(unnamedProvider.priority, 50);
    }

    void ProviderConformanceTest::providerRegistry_enforcesActiveProviderAuthorizationBoundary() {
        LocalSocketFixture fixture;
        REQUIRE_LOCAL_SOCKET_LISTENING(fixture);

        qint64                  nowMs = 1000;
        agent::ProviderRegistry registry([&nowMs] { return nowMs; });

        ConnectedSocket         highPrioritySocket = fixture.connect();
        QVERIFY(highPrioritySocket.server != nullptr);
        ConnectedSocket lowPrioritySocket = fixture.connect();
        QVERIFY(lowPrioritySocket.server != nullptr);

        registry.registerProvider(highPrioritySocket.server.get(), QJsonObject{{"name", "High"}, {"kind", "custom"}, {"priority", 90}});
        registry.registerProvider(lowPrioritySocket.server.get(), QJsonObject{{"name", "Low"}, {"kind", "custom"}, {"priority", 10}});
        QVERIFY(registry.recomputeActiveProvider());
        QCOMPARE(registry.activeProvider(), highPrioritySocket.server.get());

        QLocalSocket unknownSocket;
        QVERIFY(registry.isAuthorized(highPrioritySocket.server.get()));
        QVERIFY(!registry.isAuthorized(lowPrioritySocket.server.get()));
        QVERIFY(!registry.isAuthorized(&unknownSocket));

        QVERIFY(registry.unregisterProvider(highPrioritySocket.server.get()));
        QVERIFY(registry.unregisterProvider(lowPrioritySocket.server.get()));
        registry.recomputeActiveProvider();
        QVERIFY(!registry.hasActiveProvider());
        QVERIFY(registry.isAuthorized(&unknownSocket));
    }

    void ProviderConformanceTest::discovery_honorsDefaultDirectoryPrecedence() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());

        const QString explicitDir = QDir::cleanPath(temp.path() + "/explicit/providers.d");
        const QString configHome  = QDir::cleanPath(temp.path() + "/config-home");
        const QString configDir   = QDir::cleanPath(configHome + "/bb-auth/providers.d");
        const QString dataHome    = QDir::cleanPath(temp.path() + "/data-home");
        const QString dataDir     = QDir::cleanPath(dataHome + "/bb-auth/providers.d");
        const QString systemDir   = QDir::cleanPath(temp.path() + "/system/providers.d");

        QVERIFY(QDir().mkpath(explicitDir));
        QVERIFY(QDir().mkpath(configDir));
        QVERIFY(QDir().mkpath(dataDir));
        QVERIFY(QDir().mkpath(systemDir));

        writeFile(explicitDir + "/10-primary.json", R"({"id":"primary","name":"Primary Explicit","kind":"custom","priority":100,"exec":"/bin/true"})");
        writeFile(configDir + "/10-primary.json", R"({"id":"primary","name":"Primary Config","kind":"custom","priority":50,"exec":"/bin/true"})");
        writeFile(dataDir + "/20-secondary.json", R"({"id":"secondary","name":"Secondary Data","kind":"custom","priority":20,"exec":"/bin/true"})");
        writeFile(systemDir + "/30-third.json", R"({"id":"third","name":"Third System","kind":"custom","priority":5,"exec":"/bin/true"})");

        EnvVarGuard providerDirGuard("BB_AUTH_PROVIDER_DIR", explicitDir.toLocal8Bit());
        EnvVarGuard configHomeGuard("XDG_CONFIG_HOME", configHome.toLocal8Bit());
        EnvVarGuard dataHomeGuard("XDG_DATA_HOME", dataHome.toLocal8Bit());

        const auto  dirs   = providers::ProviderDiscovery::defaultSearchDirs(systemDir);
        const auto  result = providers::ProviderDiscovery::discover(dirs);

        QCOMPARE(result.manifests.size(), 3);
        QCOMPARE(result.manifests[0].id, QString("primary"));
        QCOMPARE(result.manifests[0].name, QString("Primary Explicit"));
        QCOMPARE(result.manifests[1].id, QString("secondary"));
        QCOMPARE(result.manifests[2].id, QString("third"));

        QVERIFY(std::any_of(result.warnings.begin(), result.warnings.end(), [](const QString& warning) { return warning.contains("duplicate id 'primary'"); }));
    }

} // namespace bb

int runProviderConformanceTests(int argc, char** argv) {
    bb::ProviderConformanceTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_provider_conformance.moc"
