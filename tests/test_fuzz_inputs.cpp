// Deterministic fuzz coverage for the two trust-boundary parsers: provider
// manifests read from disk, and IPC frames read from the daemon socket.
// Seeds are fixed so failures reproduce byte-for-byte.

#include "../src/common/Constants.hpp"
#include "../src/core/agent/MessageRouter.hpp"
#include "../src/core/ipc/IpcServer.hpp"
#include "../src/core/providers/ProviderManifest.hpp"

#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>

#include <random>

namespace bb {

    namespace {

        QByteArray validManifestJson() {
            return QByteArrayLiteral(R"({
                "id": "fuzz.provider",
                "name": "Fuzz Provider",
                "kind": "prompt",
                "priority": 50,
                "exec": "/usr/libexec/fuzz-provider",
                "args": ["--flag", "value"],
                "env": {"A": "1"},
                "capabilities": ["polkit", "keyring"],
                "autostart": true
            })");
        }

        // Fixed-seed mutation engine: truncate / flip / insert / splice /
        // interesting-byte injection over a valid baseline.
        QByteArray mutate(const QByteArray& base, std::mt19937& rng) {
            static const char interesting[] = {'"', '{',  '}',  '[',  ']',  ':',  ',',  '\\', '\0', '\n', '\r',
                                               '\t', '\x7f', '\xff', '\xfe', '0',  '9',  'e',  'E',  '+',  '-',
                                               ' ',  'n',  'u',  'l',  't',  'r',  'f',  'a',  's'};

            QByteArray out = base;
            if (out.isEmpty()) {
                out = "{}";
            }

            const int ops = 1 + static_cast<int>(rng() % 4);
            for (int i = 0; i < ops; ++i) {
                switch (rng() % 5) {
                case 0: // truncate
                    out.truncate(static_cast<qsizetype>(rng() % (out.size() + 1)));
                    break;
                case 1: { // flip a byte
                    if (out.isEmpty())
                        break;
                    const qsizetype pos = static_cast<qsizetype>(rng() % out.size());
                    out[pos]            = static_cast<char>(rng() & 0xff);
                    break;
                }
                case 2: { // insert an interesting byte
                    const qsizetype pos = static_cast<qsizetype>(rng() % (out.size() + 1));
                    out.insert(pos, interesting[rng() % (sizeof(interesting) / sizeof(interesting[0]))]);
                    break;
                }
                case 3: { // splice a random span
                    const qsizetype pos = static_cast<qsizetype>(rng() % (out.size() + 1));
                    const qsizetype len = static_cast<qsizetype>(rng() % 64);
                    QByteArray      span(static_cast<int>(len), '\0');
                    for (auto& c : span)
                        c = static_cast<char>(rng() & 0xff);
                    out.replace(pos, len, span);
                    break;
                }
                case 4: { // duplicate a slice
                    if (out.isEmpty())
                        break;
                    const qsizetype pos = static_cast<qsizetype>(rng() % out.size());
                    const qsizetype len = qMin<qsizetype>(64, out.size() - pos);
                    out.insert(pos, out.mid(pos, len));
                    break;
                }
                }
            }
            return out;
        }

        // Drains one newline-terminated reply. `pending` survives across calls so
        // replies that arrive merged in a single read are not lost.
        QByteArray readLine(QLocalSocket& socket, QByteArray& pending, int timeoutMs = 2000) {
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < timeoutMs && socket.state() == QLocalSocket::ConnectedState) {
                const qsizetype newline = pending.indexOf('\n');
                if (newline != -1) {
                    const QByteArray line = pending.left(newline);
                    pending.remove(0, newline + 1);
                    return line;
                }
                pending.append(socket.readAll());
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                socket.waitForReadyRead(20);
            }
            return QByteArray{};
        }

    } // namespace

    class FuzzInputTest : public QObject {
        Q_OBJECT

      private slots:
        void manifestMutationFuzz_neverCrashes();
        void manifestAdversarialStructures_bounded();
        void ipcGarbageFrames_serverKeepsServing();
        void ipcAdversarialFrames_handled();
    };

    // @lat: [[tests#Fuzz boundaries#Manifest mutations never crash]]
    void FuzzInputTest::manifestMutationFuzz_neverCrashes() {
        std::mt19937    rng(0xBBAC57);
        const QByteArray base = validManifestJson();

        for (int i = 0; i < 1500; ++i) {
            const QByteArray mutated = mutate(base, rng);
            const auto       result  = providers::parseProviderManifest(mutated, QStringLiteral("fuzz.json"));
            if (result.ok) {
                QVERIFY(result.manifest.isValid());
            } else {
                QVERIFY(!result.error.isEmpty());
            }
        }
    }

    // @lat: [[tests#Fuzz boundaries#Adversarial manifest structures bounded]]
    void FuzzInputTest::manifestAdversarialStructures_bounded() {
        struct Case {
            QByteArray json;
        };
        const QList<QByteArray> cases{
            QByteArray(20000, '[').append(QByteArray(20000, ']')),                        // deep nesting
            QByteArrayLiteral("{\"id\":\"") + QByteArray(200000, 'a') + QByteArrayLiteral("\"}"), // huge string
            QByteArrayLiteral("{\"id\":\"x\",\"priority\":") + QByteArray(5000, '9') + QByteArrayLiteral("}"), // huge number
            QByteArrayLiteral("{\"id\":\"\\ud800\"}"),                                    // lone surrogate
            QByteArrayLiteral("{\"id\":\"a\",\"id\":\"b\",\"name\":\"n\",\"kind\":\"prompt\",\"exec\":\"/bin/x\"}"), // dup key
            QByteArrayLiteral("{\"args\":[") + QByteArray(40000, ',').append(']'),        // comma flood
            QByteArray(60000, ' '),                                                     // whitespace flood
            QByteArrayLiteral("\xef\xbb\xbf{}"),                                        // BOM + empty object
        };

        for (const auto& json : cases) {
            const auto result = providers::parseProviderManifest(json, QStringLiteral("fuzz.json"));
            if (result.ok) {
                QVERIFY(result.manifest.isValid());
            } else {
                QVERIFY(!result.error.isEmpty());
            }
        }
    }

    // @lat: [[tests#Fuzz boundaries#Garbage IPC frames never wedge server]]
    void FuzzInputTest::ipcGarbageFrames_serverKeepsServing() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString socketPath = tempDir.path() + "/fuzz.sock";

        IpcServer              server;
        agent::MessageRouter   router;
        router.registerHandler("ping", [&server](QLocalSocket* s, const QJsonObject&) {
            server.sendJson(s, QJsonObject{{"type", "pong"}});
        });
        server.setMessageHandler([&server, &router](QLocalSocket* s, const QString& type, const QJsonObject& msg) {
            if (!router.dispatch(s, type, msg))
                server.sendJson(s, QJsonObject{{"type", "error"}, {"message", "Unknown type"}});
        });
        if (!server.start(socketPath))
            QSKIP("local sockets unavailable");

        std::mt19937 rng(0x1FCF00D);

        // Interleave garbage frames with a liveness ping on the same connection:
        // every garbage line must produce an error (or a silent drop of a
        // non-object line), never wedge or crash the server.
        QLocalSocket socket;
        socket.connectToServer(socketPath);
        QVERIFY(socket.waitForConnected(1000));

        QByteArray pending;
        for (int i = 0; i < 200; ++i) {
            QByteArray garbage(static_cast<int>(rng() % 512), '\0');
            for (auto& c : garbage)
                c = static_cast<char>(rng() & 0xff);
            for (auto& c : garbage)
                if (c == '\n')
                    c = 'x'; // keep one frame per write
            socket.write(garbage + '\n' + "{\"type\":\"ping\"}\n");
            QVERIFY(socket.waitForBytesWritten(1000));

            // First reply answers the garbage frame; the second must be pong —
            // proof the server survived and is still dispatching.
            const QByteArray reply1 = readLine(socket, pending);
            const QByteArray reply2 = readLine(socket, pending);
            QVERIFY(!reply1.isEmpty());
            QVERIFY(!reply2.isEmpty());
            const auto doc = QJsonDocument::fromJson(reply2);
            QVERIFY(doc.isObject());
            QCOMPARE(doc.object().value("type").toString(), QString("pong"));
        }
    }

    // @lat: [[tests#Fuzz boundaries#Adversarial IPC frames handled]]
    void FuzzInputTest::ipcAdversarialFrames_handled() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString socketPath = tempDir.path() + "/fuzz2.sock";

        IpcServer            server;
        agent::MessageRouter router;
        router.registerHandler("ping", [&server](QLocalSocket* s, const QJsonObject&) {
            server.sendJson(s, QJsonObject{{"type", "pong"}});
        });
        server.setMessageHandler([&server, &router](QLocalSocket* s, const QString& type, const QJsonObject& msg) {
            if (!router.dispatch(s, type, msg))
                server.sendJson(s, QJsonObject{{"type", "error"}, {"message", "Unknown type"}});
        });
        if (!server.start(socketPath))
            QSKIP("local sockets unavailable");

        const QList<QByteArray> frames{
            QByteArrayLiteral(""),                                       // empty line
            QByteArrayLiteral("null"),                                   // valid JSON, not object
            QByteArrayLiteral("[1,2,3]"),                                // array
            QByteArrayLiteral("\"ping\""),                               // bare string
            QByteArrayLiteral("{\"type\":null}"),                        // null type
            QByteArrayLiteral("{\"type\":123}"),                         // numeric type
            QByteArrayLiteral("{\"type\":\"\"}"),                        // empty type
            QByteArrayLiteral("{\"type\":\"ping\",\"type\":\"evil\"}"),  // duplicate key
            QByteArray(2000, ' ') + QByteArrayLiteral("{\"type\":\"ping\"}"), // padded frame
            QByteArrayLiteral("{\"type\":\"\\u0070ing\"}"),              // escaped type
            QByteArrayLiteral("{\"type\":\"ping\"") + QByteArray(4000, '{'), // valid head + nesting flood
        };

        for (const auto& frame : frames) {
            QLocalSocket socket;
            socket.connectToServer(socketPath);
            QVERIFY(socket.waitForConnected(1000));

            socket.write(frame + '\n');
            socket.write("{\"type\":\"ping\"}\n");
            QVERIFY(socket.waitForBytesWritten(1000));

            // First reply corresponds to the adversarial frame (or to ping if
            // the frame was consumed as padding); drain until pong or close.
            QByteArray pending;
            bool       gotPong = false;
            for (int i = 0; i < 4 && socket.state() == QLocalSocket::ConnectedState; ++i) {
                const QByteArray line = readLine(socket, pending, 1500);
                if (line.isEmpty())
                    break;
                const auto doc = QJsonDocument::fromJson(line);
                if (doc.isObject() && doc.object().value("type").toString() == "pong") {
                    gotPong = true;
                    break;
                }
            }
            QVERIFY(gotPong);
            socket.disconnectFromServer();
        }
    }

} // namespace bb

int runFuzzInputTests(int argc, char** argv) {
    bb::FuzzInputTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_fuzz_inputs.moc"
