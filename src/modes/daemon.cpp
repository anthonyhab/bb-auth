#include "daemon.hpp"
#include "../common/Paths.hpp"
#include "../core/Agent.hpp"

#include <print>

namespace modes {

    int runDaemon(QCoreApplication& app, const QString& socketPathOverride) {
        const QString socketPath = socketPathOverride.isEmpty() ? noctalia::socketPath() : socketPathOverride;

        std::print("Starting noctalia-auth daemon\n");
        std::print("Socket path: {}\n", socketPath.toStdString());

        g_pAgent = std::make_unique<CAgent>();
        return g_pAgent->start(app, socketPath) ? 0 : 1;
    }

} // namespace modes
