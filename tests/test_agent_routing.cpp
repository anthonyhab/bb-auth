#include "../src/core/agent/EventQueue.hpp"
#include "../src/core/agent/EventRouter.hpp"
#include "../src/core/agent/ProviderRegistry.hpp"

#include <QtTest/QtTest>

#include <QLocalServer>
#include <QLocalSocket>
#include <QUuid>

#include <memory>
#include <utility>
#include <vector>
#include <algorithm>

namespace bb {

    namespace {

        struct ConnectedSocket {
            std::unique_ptr<QLocalSocket> client;
            std::unique_ptr<QLocalSocket> server;
        };

        class LocalSocketFixture {
          public:
            LocalSocketFixture() {
                m_serverName = QString("bb-auth-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
                QLocalServer::removeServer(m_serverName);
                m_isListening = m_server.listen(m_serverName);
            }

            bool isListening() const {
                return m_isListening;
            }
            ConnectedSocket connect() {
                auto client = std::make_unique<QLocalSocket>();
                client->connectToServer(m_serverName);
                if (!client->waitForConnected(1000)) {
                    return ConnectedSocket{};
                }

                if (!m_server.waitForNewConnection(1000)) {
                    return ConnectedSocket{};
                }
                QLocalSocket* serverSocket = m_server.nextPendingConnection();
                if (!serverSocket) {
                    return ConnectedSocket{};
                }
                serverSocket->setParent(nullptr);
                return ConnectedSocket{std::move(client), std::unique_ptr<QLocalSocket>(serverSocket)};
            }

          private:
            QLocalServer m_server;
            QString      m_serverName;
            bool         m_isListening = false;
        };

        struct SentEvent {
            QLocalSocket* socket = nullptr;
            QString       type;
        };

        inline QJsonObject makeEvent(const QString& type) {
            return QJsonObject{{"type", type}};
        }

    } // namespace

    class AgentRoutingTest : public QObject {
        Q_OBJECT

      private slots:
        void providerRegistry_selectsHighestPriority();
        void providerRegistry_tiesBreakByMostRecentHeartbeat();
        void providerRegistry_unregActiveRecomputes();
        void providerRegistry_heartbeatUnknownReturnsFalse();
        void providerRegistry_prunesStaleAndDisconnected();

        void eventQueue_dropsOldestAtCapacity();
        void eventQueue_drainsWaitersInFifoOrder();
        void eventQueue_removeWaiterPreventsSend();

        void eventRouter_routesSessionEventsToActiveProviderOnly();
        void eventRouter_broadcastsSessionEventsWhenNoActiveProvider();
        void eventRouter_broadcastsNonSessionEventsEvenWithActiveProvider();
    };

    void AgentRoutingTest::providerRegistry_selectsHighestPriority() {
        LocalSocketFixture fixture;
        QVERIFY(fixture.isListening());

        qint64                  nowMs = 1000;
        agent::ProviderRegistry registry([&nowMs] { return nowMs; });

        ConnectedSocket         a = fixture.connect();
        QVERIFY(a.server != nullptr);

        ConnectedSocket b = fixture.connect();
        QVERIFY(b.server != nullptr);

        nowMs = 1000;
        registry.registerProvider(a.server.get(), QJsonObject{{"name", "a"}, {"kind", "a"}, {"priority", 10}});

        nowMs = 2000;
        registry.registerProvider(b.server.get(), QJsonObject{{"name", "b"}, {"kind", "b"}, {"priority", 20}});

        nowMs              = 3000;
        const bool changed = registry.recomputeActiveProvider();
        QVERIFY(changed);
        QCOMPARE(registry.activeProvider(), b.server.get());
        QVERIFY(registry.hasActiveProvider());

        const agent::UIProvider* info = registry.activeProviderInfo();
        QVERIFY(info != nullptr);
        QCOMPARE(info->priority, 20);
    }

    void AgentRoutingTest::providerRegistry_tiesBreakByMostRecentHeartbeat() {
        LocalSocketFixture fixture;
        QVERIFY(fixture.isListening());

        qint64                  nowMs = 1000;
        agent::ProviderRegistry registry([&nowMs] { return nowMs; });

        ConnectedSocket         a = fixture.connect();
        QVERIFY(a.server != nullptr);

        ConnectedSocket b = fixture.connect();
        QVERIFY(b.server != nullptr);

        nowMs = 1000;
        registry.registerProvider(a.server.get(), QJsonObject{{"name", "a"}, {"kind", "a"}, {"priority", 10}});

        nowMs = 2000;
        registry.registerProvider(b.server.get(), QJsonObject{{"name", "b"}, {"kind", "b"}, {"priority", 10}});

        nowMs = 2500;
        QVERIFY(registry.recomputeActiveProvider());
        QCOMPARE(registry.activeProvider(), b.server.get());

        nowMs = 3000;
        QVERIFY(registry.heartbeat(a.server.get()));

        nowMs = 3500;
        QVERIFY(registry.recomputeActiveProvider());
        QCOMPARE(registry.activeProvider(), a.server.get());
    }

    void AgentRoutingTest::providerRegistry_unregActiveRecomputes() {
        LocalSocketFixture fixture;
        QVERIFY(fixture.isListening());

        qint64                  nowMs = 1000;
        agent::ProviderRegistry registry([&nowMs] { return nowMs; });

        ConnectedSocket         a = fixture.connect();
        QVERIFY(a.server != nullptr);

        ConnectedSocket b = fixture.connect();
        QVERIFY(b.server != nullptr);

        nowMs = 1000;
        registry.registerProvider(a.server.get(), QJsonObject{{"name", "a"}, {"kind", "a"}, {"priority", 10}});

        nowMs = 2000;
        registry.registerProvider(b.server.get(), QJsonObject{{"name", "b"}, {"kind", "b"}, {"priority", 20}});

        nowMs = 3000;
        registry.recomputeActiveProvider();
        QCOMPARE(registry.activeProvider(), b.server.get());

        QVERIFY(registry.unregisterProvider(b.server.get()));

        nowMs = 4000;
        QVERIFY(registry.recomputeActiveProvider());
        QCOMPARE(registry.activeProvider(), a.server.get());
    }

    void AgentRoutingTest::providerRegistry_heartbeatUnknownReturnsFalse() {
        qint64                  nowMs = 0;
        agent::ProviderRegistry registry([&nowMs] { return nowMs; });

        QLocalSocket            unknown;
        QVERIFY(!registry.heartbeat(&unknown));
    }

    void AgentRoutingTest::providerRegistry_prunesStaleAndDisconnected() {
        LocalSocketFixture fixture;
        QVERIFY(fixture.isListening());

        qint64                  nowMs = 1000;
        agent::ProviderRegistry registry([&nowMs] { return nowMs; });

        ConnectedSocket         a = fixture.connect();
        QVERIFY(a.server != nullptr);

        ConnectedSocket b = fixture.connect();
        QVERIFY(b.server != nullptr);

        nowMs = 1000;
        registry.registerProvider(a.server.get(), QJsonObject{{"name", "a"}, {"kind", "a"}, {"priority", 50}});

        nowMs = 2000;
        registry.registerProvider(b.server.get(), QJsonObject{{"name", "b"}, {"kind", "b"}, {"priority", 60}});

        nowMs = 3000;
        registry.recomputeActiveProvider();
        QVERIFY(registry.contains(a.server.get()));
        QVERIFY(registry.contains(b.server.get()));

        // Disconnect b, keep a connected.
        b.client->disconnectFromServer();
        QTRY_COMPARE(b.client->state(), QLocalSocket::UnconnectedState);
        QTRY_COMPARE(b.server->state(), QLocalSocket::UnconnectedState);

        nowMs = 4000;
        QVERIFY(registry.recomputeActiveProvider());
        QVERIFY(!registry.contains(b.server.get()));
        QVERIFY(registry.contains(a.server.get()));
        QCOMPARE(registry.activeProvider(), a.server.get());

        // Make a stale.
        nowMs = 20000;
        QVERIFY(registry.recomputeActiveProvider());
        QVERIFY(!registry.contains(a.server.get()));
        QVERIFY(!registry.hasActiveProvider());
        QCOMPARE(registry.activeProvider(), nullptr);
    }

    void AgentRoutingTest::eventQueue_dropsOldestAtCapacity() {
        agent::EventQueue queue(2);

        queue.enqueue(makeEvent("e1"));
        queue.enqueue(makeEvent("e2"));
        queue.enqueue(makeEvent("e3"));

        QCOMPARE(queue.takeNext().value("type").toString(), QString("e2"));
        QCOMPARE(queue.takeNext().value("type").toString(), QString("e3"));
        QVERIFY(queue.takeNext().isEmpty());
    }

    void AgentRoutingTest::eventQueue_drainsWaitersInFifoOrder() {
        LocalSocketFixture fixture;
        QVERIFY(fixture.isListening());

        ConnectedSocket w1 = fixture.connect();
        QVERIFY(w1.server != nullptr);

        ConnectedSocket w2 = fixture.connect();
        QVERIFY(w2.server != nullptr);

        agent::EventQueue queue(10);
        queue.subscribeNext(w1.server.get());
        queue.subscribeNext(w2.server.get());

        queue.enqueue(makeEvent("e1"));
        queue.enqueue(makeEvent("e2"));

        std::vector<SentEvent> sent;
        queue.drainToWaiters([&sent](QLocalSocket* socket, const QJsonObject& event) { sent.push_back(SentEvent{socket, event.value("type").toString()}); });

        QCOMPARE(sent.size(), static_cast<size_t>(2));
        QCOMPARE(sent[0].socket, w1.server.get());
        QCOMPARE(sent[0].type, QString("e1"));
        QCOMPARE(sent[1].socket, w2.server.get());
        QCOMPARE(sent[1].type, QString("e2"));
        QVERIFY(queue.isEmpty());

        // Remaining waiter stays registered until serviced.
        ConnectedSocket w3 = fixture.connect();
        QVERIFY(w3.server != nullptr);

        queue.subscribeNext(w3.server.get());
        queue.subscribeNext(w2.server.get());

        queue.enqueue(makeEvent("e3"));
        sent.clear();
        queue.drainToWaiters([&sent](QLocalSocket* socket, const QJsonObject& event) { sent.push_back(SentEvent{socket, event.value("type").toString()}); });

        QCOMPARE(sent.size(), static_cast<size_t>(1));
        QCOMPARE(sent[0].socket, w3.server.get());
        QCOMPARE(sent[0].type, QString("e3"));

        queue.enqueue(makeEvent("e4"));
        sent.clear();
        queue.drainToWaiters([&sent](QLocalSocket* socket, const QJsonObject& event) { sent.push_back(SentEvent{socket, event.value("type").toString()}); });

        QCOMPARE(sent.size(), static_cast<size_t>(1));
        QCOMPARE(sent[0].socket, w2.server.get());
        QCOMPARE(sent[0].type, QString("e4"));
    }

    void AgentRoutingTest::eventQueue_removeWaiterPreventsSend() {
        LocalSocketFixture fixture;
        QVERIFY(fixture.isListening());

        ConnectedSocket w1 = fixture.connect();
        QVERIFY(w1.server != nullptr);

        agent::EventQueue queue(10);
        queue.subscribeNext(w1.server.get());
        queue.removeWaiter(w1.server.get());

        queue.enqueue(makeEvent("e1"));

        std::vector<SentEvent> sent;
        queue.drainToWaiters([&sent](QLocalSocket* socket, const QJsonObject& event) { sent.push_back(SentEvent{socket, event.value("type").toString()}); });

        QVERIFY(sent.empty());
    }

    void AgentRoutingTest::eventRouter_routesSessionEventsToActiveProviderOnly() {
        LocalSocketFixture fixture;
        QVERIFY(fixture.isListening());

        qint64                  nowMs = 1000;
        agent::ProviderRegistry registry([&nowMs] { return nowMs; });
        agent::EventQueue       queue(10);
        agent::EventRouter      router(registry, queue);

        ConnectedSocket         provider = fixture.connect();
        QVERIFY(provider.server != nullptr);

        ConnectedSocket sub1 = fixture.connect();
        QVERIFY(sub1.server != nullptr);

        ConnectedSocket sub2 = fixture.connect();
        QVERIFY(sub2.server != nullptr);

        ConnectedSocket waiter = fixture.connect();
        QVERIFY(waiter.server != nullptr);

        nowMs = 1000;
        registry.registerProvider(provider.server.get(), QJsonObject{{"name", "provider"}, {"kind", "provider"}, {"priority", 50}});

        nowMs = 1100;
        registry.recomputeActiveProvider();
        QCOMPARE(registry.activeProvider(), provider.server.get());

        queue.subscribeNext(waiter.server.get());

        std::vector<SentEvent>     sent;
        const QList<QLocalSocket*> subscribers{sub1.server.get(), sub2.server.get()};
        router.route(makeEvent("session.created"), subscribers,
                     [&sent](QLocalSocket* socket, const QJsonObject& event) { sent.push_back(SentEvent{socket, event.value("type").toString()}); });

        QCOMPARE(sent.size(), static_cast<size_t>(2));
        QCOMPARE(sent[0].socket, provider.server.get());
        QCOMPARE(sent[0].type, QString("session.created"));
        QCOMPARE(sent[1].socket, waiter.server.get());
        QCOMPARE(sent[1].type, QString("session.created"));
    }

    void AgentRoutingTest::eventRouter_broadcastsSessionEventsWhenNoActiveProvider() {
        LocalSocketFixture fixture;
        QVERIFY(fixture.isListening());

        qint64                  nowMs = 0;
        agent::ProviderRegistry registry([&nowMs] { return nowMs; });
        agent::EventQueue       queue(10);
        agent::EventRouter      router(registry, queue);

        ConnectedSocket         sub1 = fixture.connect();
        QVERIFY(sub1.server != nullptr);

        ConnectedSocket sub2 = fixture.connect();
        QVERIFY(sub2.server != nullptr);

        ConnectedSocket waiter = fixture.connect();
        QVERIFY(waiter.server != nullptr);

        queue.subscribeNext(waiter.server.get());

        std::vector<SentEvent>     sent;
        const QList<QLocalSocket*> subscribers{sub1.server.get(), sub2.server.get()};
        router.route(makeEvent("session.updated"), subscribers,
                     [&sent](QLocalSocket* socket, const QJsonObject& event) { sent.push_back(SentEvent{socket, event.value("type").toString()}); });

        QCOMPARE(sent.size(), static_cast<size_t>(3));
        QCOMPARE(sent[0].socket, sub1.server.get());
        QCOMPARE(sent[1].socket, sub2.server.get());
        QCOMPARE(sent[2].socket, waiter.server.get());
    }

    void AgentRoutingTest::eventRouter_broadcastsNonSessionEventsEvenWithActiveProvider() {
        LocalSocketFixture fixture;
        QVERIFY(fixture.isListening());

        qint64                  nowMs = 1000;
        agent::ProviderRegistry registry([&nowMs] { return nowMs; });
        agent::EventQueue       queue(10);
        agent::EventRouter      router(registry, queue);

        ConnectedSocket         provider = fixture.connect();
        QVERIFY(provider.server != nullptr);

        ConnectedSocket sub1 = fixture.connect();
        QVERIFY(sub1.server != nullptr);

        ConnectedSocket sub2 = fixture.connect();
        QVERIFY(sub2.server != nullptr);

        ConnectedSocket waiter = fixture.connect();
        QVERIFY(waiter.server != nullptr);

        nowMs = 1000;
        registry.registerProvider(provider.server.get(), QJsonObject{{"name", "provider"}, {"kind", "provider"}, {"priority", 50}});

        nowMs = 1100;
        registry.recomputeActiveProvider();

        queue.subscribeNext(waiter.server.get());

        std::vector<SentEvent>     sent;
        const QList<QLocalSocket*> subscribers{sub1.server.get(), sub2.server.get()};
        router.route(makeEvent("ui.active"), subscribers,
                     [&sent](QLocalSocket* socket, const QJsonObject& event) { sent.push_back(SentEvent{socket, event.value("type").toString()}); });

        QCOMPARE(sent.size(), static_cast<size_t>(3));
        QCOMPARE(sent[0].socket, sub1.server.get());
        QCOMPARE(sent[1].socket, sub2.server.get());
        QCOMPARE(sent[2].socket, waiter.server.get());

        // Active provider is not implicitly subscribed; it should not receive non-session broadcasts unless included.
        QVERIFY(std::none_of(sent.begin(), sent.end(), [&](const SentEvent& e) { return e.socket == provider.server.get(); }));
    }

} // namespace bb

int runAgentRoutingTests(int argc, char** argv) {
    bb::AgentRoutingTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_agent_routing.moc"
