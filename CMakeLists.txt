cmake_minimum_required(VERSION 3.16)

# Get version
file(READ "${CMAKE_SOURCE_DIR}/VERSION" VER_RAW)
string(STRIP ${VER_RAW} VER)

project(bb-auth VERSION ${VER} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network DBus Widgets Test)
find_package(PkgConfig REQUIRED)

if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Werror=format-security)
    add_link_options(-Wl,-z,relro -Wl,-z,now)
endif()

# Polkit dependencies
pkg_check_modules(
  polkit_deps
  REQUIRED
  IMPORTED_TARGET
  polkit-agent-1
  polkit-qt6-1)

# GCR/GLib dependencies for keyring mode
pkg_check_modules(
  gcr_deps
  REQUIRED
  IMPORTED_TARGET
  gcr-4
  gio-2.0
  json-glib-1.0)

qt_standard_project_setup(REQUIRES 6.5)
enable_testing()

# Unified bb-auth binary
qt_add_executable(bb-auth
    # Main entry point
    src/main.cpp

    # Common utilities
    src/common/IpcClient.cpp
    src/common/IpcClient.hpp
    src/common/Paths.cpp
    src/common/Paths.hpp

    # Core components
    src/core/Session.hpp
    src/core/Session.cpp
    src/core/Agent.cpp
    src/core/Agent.hpp
    src/core/agent/EventQueue.cpp
    src/core/agent/EventQueue.hpp
    src/core/agent/ProviderRegistry.cpp
    src/core/agent/ProviderRegistry.hpp
    src/core/agent/EventRouter.cpp
    src/core/agent/EventRouter.hpp
    src/core/agent/SessionStore.cpp
    src/core/agent/SessionStore.hpp
    src/core/agent/MessageRouter.cpp
    src/core/agent/MessageRouter.hpp
    src/core/PolkitListener.hpp
    src/core/PolkitListener.cpp
    src/core/RequestContext.hpp
    src/core/RequestContext.cpp
    src/core/ipc/IpcServer.cpp

    # Managers
    src/core/managers/KeyringManager.cpp
    src/core/managers/KeyringManager.hpp
    src/core/managers/PinentryManager.cpp
    src/core/managers/PinentryManager.hpp
    src/core/managers/RequestTypes.hpp

    # Mode handlers
    src/modes/daemon.cpp
    src/modes/daemon.hpp
    src/modes/keyring.cpp
    src/modes/keyring.hpp
    src/modes/pinentry.cpp
    src/modes/pinentry.hpp

    # Keyring prompter (C code, compiled into main binary)
    src/keyring-prompter/main.c
    src/keyring-prompter/bb-prompt.c
    src/keyring-prompter/bb-prompt.h
    src/keyring-prompter/ipc-client.c
    src/keyring-prompter/ipc-client.h
)

target_link_libraries(bb-auth
    PRIVATE
        Qt6::Core
        Qt6::Network
        Qt6::DBus
        PkgConfig::polkit_deps
        PkgConfig::gcr_deps
)
target_compile_definitions(bb-auth PRIVATE BB_AUTH_VERSION="${VER}")

include(GNUInstallDirs)

set(LIBEXECDIR ${CMAKE_INSTALL_FULL_LIBEXECDIR})
set(DATADIR ${CMAKE_INSTALL_FULL_DATADIR})

# Configure service files
configure_file(assets/bb-auth.service.in bb-auth.service @ONLY)
configure_file(assets/bb-auth-dbus.service.in bb-auth-dbus.service @ONLY)
configure_file(assets/org.gnome.keyring.SystemPrompter.service.in org.gnome.keyring.SystemPrompter.service @ONLY)
configure_file(assets/bb-auth-bootstrap.sh.in bb-auth-bootstrap @ONLY)

# Install main binary
install(TARGETS bb-auth
        DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})

qt_add_executable(bb-auth-fallback
    src/fallback/main.cpp
    src/fallback/FallbackClient.cpp
    src/fallback/FallbackClient.hpp
    src/fallback/FallbackWindow.cpp
    src/fallback/FallbackWindow.hpp
    src/fallback/prompt/TextNormalize.cpp
    src/fallback/prompt/TextNormalize.hpp
    src/fallback/prompt/PromptHeuristics.cpp
    src/fallback/prompt/PromptHeuristics.hpp
    src/fallback/prompt/PromptExtractors.cpp
    src/fallback/prompt/PromptExtractors.hpp
    src/fallback/prompt/PromptModel.hpp
    src/fallback/prompt/PromptModelBuilder.cpp
    src/fallback/prompt/PromptModelBuilder.hpp
)

target_link_libraries(bb-auth-fallback
    PRIVATE
        Qt6::Core
        Qt6::Network
        Qt6::Widgets
)

install(TARGETS bb-auth-fallback
        DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
qt_add_executable(bb-auth-tests
    tests/test_session_info.cpp
    tests/test_fallback_touch_model.cpp
    tests/test_agent_routing.cpp

    src/core/Session.cpp
    src/core/Session.hpp
    src/core/agent/EventQueue.cpp
    src/core/agent/EventQueue.hpp
    src/core/agent/ProviderRegistry.cpp
    src/core/agent/ProviderRegistry.hpp
    src/core/agent/EventRouter.cpp
    src/core/agent/EventRouter.hpp

    src/fallback/prompt/TextNormalize.cpp
    src/fallback/prompt/TextNormalize.hpp
    src/fallback/prompt/PromptHeuristics.cpp
    src/fallback/prompt/PromptHeuristics.hpp
    src/fallback/prompt/PromptExtractors.cpp
    src/fallback/prompt/PromptExtractors.hpp
    src/fallback/prompt/PromptModel.hpp
    src/fallback/prompt/PromptModelBuilder.cpp
    src/fallback/prompt/PromptModelBuilder.hpp
)

target_link_libraries(bb-auth-tests
    PRIVATE
        Qt6::Test
        Qt6::Core
        Qt6::Widgets
        Qt6::Network
)

add_test(NAME bb-auth-tests COMMAND bb-auth-tests)

install(PROGRAMS ${CMAKE_BINARY_DIR}/bb-auth-bootstrap
        DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})

# Install symlinks for protocol compatibility
install(CODE "
    execute_process(
        COMMAND \${CMAKE_COMMAND} -E create_symlink
            bb-auth
            bb-keyring-prompter
        WORKING_DIRECTORY \"\$ENV{DESTDIR}${CMAKE_INSTALL_FULL_LIBEXECDIR}\"
    )
    execute_process(
        COMMAND \${CMAKE_COMMAND} -E create_symlink
            bb-auth
            pinentry-bb
        WORKING_DIRECTORY \"\$ENV{DESTDIR}${CMAKE_INSTALL_FULL_LIBEXECDIR}\"
    )
")

# Install systemd user service
install(FILES ${CMAKE_BINARY_DIR}/bb-auth.service
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/user")

# Install D-Bus service file
install(FILES ${CMAKE_BINARY_DIR}/bb-auth-dbus.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services
        RENAME org.bb.auth.service)

# Install keyring prompter D-Bus service file template
install(FILES ${CMAKE_BINARY_DIR}/org.gnome.keyring.SystemPrompter.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/bb-auth)

# Install migration script (temporary - will be removed in future release)
install(PROGRAMS ${CMAKE_SOURCE_DIR}/scripts/bb-auth-migrate.sh
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        RENAME bb-auth-migrate)

# Uninstall target
add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_INSTALL_FULL_LIBEXECDIR}/bb-auth"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_INSTALL_FULL_LIBEXECDIR}/bb-auth-fallback"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_INSTALL_FULL_LIBEXECDIR}/bb-auth-bootstrap"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_INSTALL_FULL_LIBEXECDIR}/bb-keyring-prompter"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_INSTALL_FULL_LIBEXECDIR}/pinentry-bb"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_INSTALL_FULL_LIBDIR}/systemd/user/bb-auth.service"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_INSTALL_FULL_DATADIR}/dbus-1/services/org.bb.auth.service"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_INSTALL_FULL_DATADIR}/bb-auth/org.gnome.keyring.SystemPrompter.service"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_INSTALL_FULL_BINDIR}/bb-auth-migrate"
    COMMENT "Uninstalling bb-auth..."
)
