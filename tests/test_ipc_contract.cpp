#include "../src/common/Constants.hpp"
#include "../src/core/agent/MessageRouter.hpp"
#include "../src/core/ipc/IpcServer.hpp"

#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>

namespace bb {

    namespace {

#define REQUIRE_LOCAL_SOCKET_LISTENING(fixture)                                                                                                                  \
    do {                                                                                                                                                        \
        if (!(fixture).start()) {                                                                                                                               \
            QSKIP(qPrintable(QStringLiteral("Skipping local-socket-dependent test: %1").arg((fixture).errorString())));                                        \
        }                                                                                                                                                       \
    } while (false)

        class IpcContractFixture {
          public:
            bool start() {
                if (!m_tempDir.isValid()) {
                    m_error = "failed to create temporary directory";
                    return false;
                }

                m_socketPath = m_tempDir.path() + "/ipc-contract.sock";
                QLocalServer::removeServer(m_socketPath);

                QLocalServer probe;
                if (!probe.listen(m_socketPath)) {
                    m_error = probe.errorString();
                    return false;
                }
                probe.close();
                QLocalServer::removeServer(m_socketPath);

                m_server.setMessageHandler([this](QLocalSocket* socket, const QString& type, const QJsonObject& msg) {
                    if (!m_router.dispatch(socket, type, msg)) {
                        m_server.sendJson(socket, QJsonObject{{"type", "error"}, {"message", "Unknown type"}});
                    }
                });
                m_router.registerHandler("ping", [this](QLocalSocket* socket, const QJsonObject&) { m_server.sendJson(socket, QJsonObject{{"type", "pong"}}); });

                if (!m_server.start(m_socketPath)) {
                    m_error = "failed to start ipc server";
                    return false;
                }

                m_client.connectToServer(m_socketPath);
                if (!m_client.waitForConnected(1000)) {
                    m_error = m_client.errorString();
                    return false;
                }

                return true;
            }

            QString errorString() const {
                return m_error;
            }

            QLocalSocket& client() {
                return m_client;
            }

            QJsonObject readJsonLine(int timeoutMs = 1000) {
                QByteArray   line;
                QElapsedTimer timer;
                timer.start();

                while (timer.elapsed() < timeoutMs) {
                    line.append(m_client.readAll());
                    if (line.contains('\n')) {
                        break;
                    }

                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                    m_client.waitForReadyRead(20);
                }

                if (!line.contains('\n')) {
                    return QJsonObject{};
                }

                const qsizetype newline = line.indexOf('\n');
                const auto      json    = QJsonDocument::fromJson(line.left(newline).trimmed());
                return json.isObject() ? json.object() : QJsonObject{};
            }

          private:
            bb::IpcServer         m_server;
            bb::agent::MessageRouter m_router;
            QTemporaryDir         m_tempDir;
            QString               m_socketPath;
            QString               m_error;
            QLocalSocket          m_client;
        };

    } // namespace

    class IpcContractTest : public QObject {
        Q_OBJECT

      private slots:
        void invalidJson_returnsError();
        void missingType_returnsError();
        void unknownType_returnsError();
        void oversizedBufferedInput_disconnectsClient();
    };

    void IpcContractTest::invalidJson_returnsError() {
        IpcContractFixture fixture;
        REQUIRE_LOCAL_SOCKET_LISTENING(fixture);

        auto& socket = fixture.client();
        QVERIFY(socket.write("{\"type\":\n") > 0);
        QVERIFY(socket.waitForBytesWritten(1000));

        const auto reply = fixture.readJsonLine();
        QVERIFY(!reply.isEmpty());
        QCOMPARE(reply.value("type").toString(), QString("error"));
        QCOMPARE(reply.value("message").toString(), QString("Invalid JSON"));
    }

    void IpcContractTest::missingType_returnsError() {
        IpcContractFixture fixture;
        REQUIRE_LOCAL_SOCKET_LISTENING(fixture);

        auto& socket = fixture.client();
        QVERIFY(socket.write("{\"hello\":\"world\"}\n") > 0);
        QVERIFY(socket.waitForBytesWritten(1000));

        const auto reply = fixture.readJsonLine();
        QVERIFY(!reply.isEmpty());
        QCOMPARE(reply.value("type").toString(), QString("error"));
        QCOMPARE(reply.value("message").toString(), QString("Missing type field"));
    }

    void IpcContractTest::unknownType_returnsError() {
        IpcContractFixture fixture;
        REQUIRE_LOCAL_SOCKET_LISTENING(fixture);

        auto& socket = fixture.client();
        QVERIFY(socket.write("{\"type\":\"unknown.event\"}\n") > 0);
        QVERIFY(socket.waitForBytesWritten(1000));

        const auto reply = fixture.readJsonLine();
        QVERIFY(!reply.isEmpty());
        QCOMPARE(reply.value("type").toString(), QString("error"));
        QCOMPARE(reply.value("message").toString(), QString("Unknown type"));
    }

    void IpcContractTest::oversizedBufferedInput_disconnectsClient() {
        IpcContractFixture fixture;
        REQUIRE_LOCAL_SOCKET_LISTENING(fixture);

        auto&      socket    = fixture.client();
        QByteArray oversized(static_cast<qsizetype>(MAX_MESSAGE_SIZE + 1), 'x');
        QVERIFY(socket.write(oversized) == oversized.size());
        QVERIFY(socket.waitForBytesWritten(1000));

        QTRY_COMPARE(socket.state(), QLocalSocket::UnconnectedState);
    }

} // namespace bb

int runIpcContractTests(int argc, char** argv) {
    bb::IpcContractTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_ipc_contract.moc"
