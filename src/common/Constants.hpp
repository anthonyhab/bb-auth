#pragma once

#include <chrono>
#include <cstddef>

namespace bb {

    // IPC configuration
    inline constexpr std::size_t MAX_MESSAGE_SIZE       = 64 * 1024; // 64 KiB

    // Largest provider manifest the daemon will read off disk before parsing. Bounds a
    // hostile or accidentally-huge manifest in a user-writable providers.d directory from
    // being slurped whole into memory (F6). Same ceiling as an IPC frame.
    inline constexpr std::size_t MAX_MANIFEST_SIZE       = 64 * 1024; // 64 KiB

    // A client that connects and sends a partial frame (no terminating newline) must
    // complete it within this window or be disconnected, so a slow-drip peer cannot pin a
    // buffer slot indefinitely (F6). Long-lived idle connections with no pending bytes
    // (e.g. a subscribed provider between heartbeats) are unaffected.
    inline constexpr int         INCOMPLETE_FRAME_TIMEOUT_MS = 10 * 1000;
    inline constexpr int         IPC_CONNECT_TIMEOUT_MS = 1000;
    inline constexpr int         IPC_READ_TIMEOUT_MS    = 1000;
    inline constexpr int         IPC_WRITE_TIMEOUT_MS   = 1000;

    // Pinentry timeouts
    inline constexpr int PINENTRY_REQUEST_TIMEOUT_MS = 5 * 60 * 1000; // 5 minutes
    inline constexpr int PINENTRY_RESULT_TIMEOUT_MS  = 10 * 1000;     // wait for terminal result after submit

    // Authentication
    inline constexpr int MAX_AUTH_RETRIES = 3;

    namespace json {
        // Keys
        inline constexpr const char* KEY_TYPE         = "type";
        inline constexpr const char* KEY_VERSION      = "version";
        inline constexpr const char* KEY_CAPABILITIES = "capabilities";
        inline constexpr const char* KEY_BOOTSTRAP    = "bootstrap";
        inline constexpr const char* KEY_PROVIDER     = "provider";
        inline constexpr const char* KEY_ID           = "id";
        inline constexpr const char* KEY_NAME         = "name";
        inline constexpr const char* KEY_KIND         = "kind";
        inline constexpr const char* KEY_PRIORITY     = "priority";
        inline constexpr const char* KEY_ACTIVE       = "active";
        inline constexpr const char* KEY_MESSAGE      = "message";

        // Values
        inline constexpr const char* VAL_PING          = "ping";
        inline constexpr const char* VAL_OK            = "ok";
        inline constexpr const char* VAL_ERROR         = "error";
        inline constexpr const char* VAL_UI_ACTIVE     = "ui.active";
        inline constexpr const char* VAL_UI_REGISTERED = "ui.registered";
        inline constexpr const char* VAL_SUBSCRIBED    = "subscribed";
        inline constexpr const char* VAL_PONG          = "pong";
        inline constexpr const char* VAL_POLKIT        = "polkit";
        inline constexpr const char* VAL_KEYRING       = "keyring";
        inline constexpr const char* VAL_PINENTRY      = "pinentry";
        inline constexpr const char* VAL_FINGERPRINT   = "fingerprint";
        inline constexpr const char* VAL_FIDO2         = "fido2";
    }

} // namespace bb
