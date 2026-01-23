#pragma once

#include <chrono>
#include <cstddef>

namespace noctalia {

    // IPC configuration
    inline constexpr std::size_t MAX_MESSAGE_SIZE       = 64 * 1024; // 64 KiB
    inline constexpr int         IPC_CONNECT_TIMEOUT_MS = 1000;
    inline constexpr int         IPC_READ_TIMEOUT_MS    = 1000;
    inline constexpr int         IPC_WRITE_TIMEOUT_MS   = 1000;

    // Pinentry timeouts
    inline constexpr int PINENTRY_REQUEST_TIMEOUT_MS      = 5 * 60 * 1000; // 5 minutes
    inline constexpr int PINENTRY_DEFERRED_DELAY_MS       = 300;
    inline constexpr int PINENTRY_DEFERRED_DELAY_RETRY_MS = 3000;

    // Authentication
    inline constexpr int MAX_AUTH_RETRIES = 3;

} // namespace noctalia
