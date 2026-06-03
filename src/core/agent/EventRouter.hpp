#pragma once

#include "EventQueue.hpp"
#include "ProviderRegistry.hpp"

#include <QJsonObject>
#include <QList>
#include <QLocalSocket>

namespace bb::agent {

    class EventRouter {
      public:
        EventRouter(ProviderRegistry& providerRegistry, EventQueue& eventQueue);

        template <typename SendFn>
        void route(const QJsonObject& event, const QList<QLocalSocket*>& subscribers, SendFn sendFn) {
            if (isSessionEventForProviderRouting(event)) {
                // Session events carry the prompt text and the requestor's identity. They
                // are delivered ONLY to the trusted active provider — never broadcast to
                // arbitrary same-UID subscribers, and never queued for `next` pull-waiters
                // (F4). When no provider is active the event is dropped from live delivery:
                // a provider that registers later still sees open sessions via
                // subscribe-replay from the session store, so nothing leaks and nothing is
                // stranded.
                if (m_providerRegistry.hasActiveProvider()) {
                    QLocalSocket* activeProvider = m_providerRegistry.activeProvider();
                    if (activeProvider && activeProvider->isValid()) {
                        sendFn(activeProvider, event);
                    }
                }
                return;
            }

            // Non-session events (e.g. ui.active) carry no secret context and are broadcast
            // to subscribers and made available to `next` waiters.
            for (QLocalSocket* subscriber : subscribers) {
                if (subscriber && subscriber->isValid()) {
                    sendFn(subscriber, event);
                }
            }

            m_eventQueue.enqueue(event);
            m_eventQueue.drainToWaiters(sendFn);
        }

      private:
        bool              isSessionEventForProviderRouting(const QJsonObject& event) const;

        ProviderRegistry& m_providerRegistry;
        EventQueue&       m_eventQueue;
    };

} // namespace bb::agent
