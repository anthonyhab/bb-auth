#include "Paths.hpp"

#include <QStandardPaths>

namespace noctalia {

    QString socketPath() {
        const auto runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        return runtimeDir + QStringLiteral("/noctalia-auth.sock");
    }

} // namespace noctalia
