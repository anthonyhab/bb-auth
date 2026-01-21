#pragma once

#include <QString>

namespace noctalia {

    // Returns the default socket path: $XDG_RUNTIME_DIR/noctalia-auth.sock
    QString socketPath();

} // namespace noctalia
