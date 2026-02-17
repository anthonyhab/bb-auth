#include "daemon.hpp"
#include "../common/Paths.hpp"
#include "../core/Agent.hpp"

#include <print>

namespace modes {

    int runDaemon(QCoreApplication& app, const QString& socketPathOverride) {
        const QString socketPath = socketPathOverride.isEmpty() ? bb::socketPath() : socketPathOverride;

        std::print("Starting bb-auth daemon\n");
        std::print("Socket path: {}\n", socketPath.toStdString());

        g_pAgent = std::make_unique<CAgent>();
        if (!g_pAgent->start(app, socketPath)) {
            // PolkitQt listener teardown can crash after failed register attempts.
            // Leak the agent on this short-lived error path and exit cleanly.
            (void)g_pAgent.release();
            return 1;
        }

        return 0;
    }

} // namespace modes
