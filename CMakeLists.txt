cmake_minimum_required(VERSION 3.16)

# Get version
file(READ "${CMAKE_SOURCE_DIR}/VERSION" VER_RAW)
string(STRIP ${VER_RAW} VER)

project(bb-auth VERSION ${VER} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network DBus Widgets Test)
find_package(PkgConfig REQUIRED)

if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Werror=format-security)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(BB_AUTH_TOOLCHAIN_FLAGS
            "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS} ${CMAKE_C_FLAGS_RELEASE} ${CMAKE_CXX_FLAGS_RELEASE}")
        if(NOT BB_AUTH_TOOLCHAIN_FLAGS MATCHES "_FORTIFY_SOURCE")
            add_compile_definitions(_FORTIFY_SOURCE=2)
        endif()
    endif()
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

qt_standard_project_setup(REQUIRES 6.4)
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
    src/core/agent/ProviderTrustStore.cpp
    src/core/agent/ProviderTrustStore.hpp
    src/core/agent/EventRouter.cpp
    src/core/agent/EventRouter.hpp
    src/core/agent/SessionStore.cpp
    src/core/agent/SessionStore.hpp
    src/core/agent/MessageRouter.cpp
    src/core/agent/MessageRouter.hpp
    src/core/agent/IntentStore.cpp
    src/core/agent/IntentStore.hpp
    src/core/PolkitListener.hpp
    src/core/PolkitListener.cpp
    src/core/RequestContext.hpp
    src/core/RequestContext.cpp
    src/common/ProcAgent.cpp
    src/core/ipc/IpcServer.cpp

    # Provider plumbing
    src/core/providers/ProviderManifest.cpp
    src/core/providers/ProviderManifest.hpp
    src/core/providers/ProviderDiscovery.cpp
    src/core/providers/ProviderDiscovery.hpp
    src/core/providers/ProviderLauncher.cpp
    src/core/providers/ProviderLauncher.hpp

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
target_compile_definitions(bb-auth
    PRIVATE
        BB_AUTH_VERSION="${VER}"
        BB_AUTH_PROVIDER_SYSTEM_DIR="${CMAKE_INSTALL_FULL_DATADIR}/bb-auth/providers.d")

# Configurable gcr-prompter path
set(BB_AUTH_GCR_PROMPTER_BINARY "/usr/lib/gcr-prompter" CACHE STRING "Path to the system gcr-prompter binary")
target_compile_definitions(bb-auth PRIVATE GCR_PROMPTER_BINARY="\\"${BB_AUTH_GCR_PROMPTER_BINARY}\\"")

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

# bb-auth-agent — one Qt6::Core binary behind every agent-facing name,
# dispatched by argv[0]: bb-auth-intent-hook (harness hook), aisudo
# (voluntary CLI), bb-auth-agents (harness installer), and sudo/doas/pkexec
# (agent-gated PATH shims under <libexec>/bb-auth-shims; <libexec>/bb-auth is
# the daemon binary, so the dir cannot nest there). No interpreter on the
# sudo path.
set(BB_AUTH_SHIM_DIR "${CMAKE_INSTALL_FULL_LIBEXECDIR}/bb-auth-shims")
set(BB_AUTH_INTENT_HOOK_PATH "${CMAKE_INSTALL_FULL_LIBEXECDIR}/bb-auth-intent-hook")
set(BB_AUTH_INTEGRATIONS_DIR "${CMAKE_INSTALL_FULL_DATADIR}/bb-auth/integrations")
qt_add_executable(bb-auth-agent
        integrations/agent/main.cpp
        integrations/agent/Common.cpp
        integrations/agent/Translate.cpp
        integrations/agent/OrderedJson.cpp
        integrations/agent/Hook.cpp
        integrations/agent/Aisudo.cpp
        integrations/agent/Installer.cpp
        src/common/ProcAgent.cpp)
target_link_libraries(bb-auth-agent PRIVATE Qt6::Core)
target_compile_definitions(bb-auth-agent PRIVATE
        BB_AUTH_VERSION="${VER}"
        BB_AUTH_SHIM_DIR="${BB_AUTH_SHIM_DIR}"
        BB_AUTH_INTENT_HOOK_PATH="${BB_AUTH_INTENT_HOOK_PATH}"
        BB_AUTH_INTEGRATIONS_DIR="${BB_AUTH_INTEGRATIONS_DIR}")
install(TARGETS bb-auth-agent
        DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})

# Build-tree names so tests exercise the argv0 dispatch exactly as installed.
foreach(_bb_auth_name bb-auth-intent-hook aisudo bb-auth-agents)
    add_custom_command(TARGET bb-auth-agent POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink bb-auth-agent ${CMAKE_BINARY_DIR}/${_bb_auth_name})
endforeach()

# Installed names. Relative targets keep every link correct under any prefix.
file(RELATIVE_PATH BB_AUTH_BIN_TO_AGENT
     "/${CMAKE_INSTALL_BINDIR}" "/${CMAKE_INSTALL_LIBEXECDIR}/bb-auth-agent")
install(CODE "
    if(\"${CMAKE_INSTALL_LIBEXECDIR}\" MATCHES \"^/\")
        set(_libexec \"\$ENV{DESTDIR}${CMAKE_INSTALL_LIBEXECDIR}\")
        set(_bindir \"\$ENV{DESTDIR}${CMAKE_INSTALL_BINDIR}\")
    else()
        set(_libexec \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBEXECDIR}\")
        set(_bindir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")
    endif()
    file(MAKE_DIRECTORY \"\${_libexec}/bb-auth-shims\" \"\${_bindir}\")
    file(REMOVE \"\${_libexec}/bb-auth-intent-hook\" \"\${_bindir}/aisudo\" \"\${_bindir}/bb-auth-agents\")
    file(CREATE_LINK bb-auth-agent \"\${_libexec}/bb-auth-intent-hook\" SYMBOLIC)
    file(CREATE_LINK \"${BB_AUTH_BIN_TO_AGENT}\" \"\${_bindir}/aisudo\" SYMBOLIC)
    file(CREATE_LINK \"${BB_AUTH_BIN_TO_AGENT}\" \"\${_bindir}/bb-auth-agents\" SYMBOLIC)
    foreach(_tool sudo doas pkexec)
        file(REMOVE \"\${_libexec}/bb-auth-shims/\${_tool}\")
        file(CREATE_LINK ../bb-auth-agent \"\${_libexec}/bb-auth-shims/\${_tool}\" SYMBOLIC)
    endforeach()
")

# opencode plugin, pi extension, and integration docs — runtime drop-ins the
# installer symlinks into each harness.
install(FILES integrations/opencode/bb-auth-plugin.js
        DESTINATION ${CMAKE_INSTALL_DATADIR}/bb-auth/integrations/opencode)
install(FILES integrations/pi/bb-auth-extension.ts
        DESTINATION ${CMAKE_INSTALL_DATADIR}/bb-auth/integrations/pi)
install(FILES integrations/agent/README.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/bb-auth/integrations)

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
    tests/test_fallback_window_states.cpp
    tests/test_agent_routing.cpp
    tests/test_ipc_contract.cpp
    tests/test_provider_manifest.cpp
    tests/test_provider_discovery.cpp
    tests/test_provider_launcher.cpp
    tests/test_provider_conformance.cpp
    tests/test_provider_trust.cpp
    tests/test_text_normalize.cpp
    tests/test_session_store.cpp
    tests/test_classify_request.cpp
    tests/test_prompt_extractors.cpp
    tests/test_request_context.cpp
    tests/test_intent_store.cpp
    tests/test_fuzz_inputs.cpp
    tests/test_prompt_snapshots.cpp

    src/core/Session.cpp
    src/core/Session.hpp
    src/core/RequestContext.cpp
    src/common/ProcAgent.cpp
    src/core/RequestContext.hpp
    src/core/agent/EventQueue.cpp
    src/core/agent/EventQueue.hpp
    src/core/agent/ProviderRegistry.cpp
    src/core/agent/ProviderRegistry.hpp
    src/core/agent/ProviderTrustStore.cpp
    src/core/agent/ProviderTrustStore.hpp
    src/core/agent/EventRouter.cpp
    src/core/agent/EventRouter.hpp
    src/core/agent/SessionStore.cpp
    src/core/agent/SessionStore.hpp
    src/core/agent/MessageRouter.cpp
    src/core/agent/MessageRouter.hpp
    src/core/agent/IntentStore.cpp
    src/core/agent/IntentStore.hpp
    src/core/ipc/IpcServer.cpp
    src/core/providers/ProviderManifest.cpp
    src/core/providers/ProviderManifest.hpp
    src/core/providers/ProviderDiscovery.cpp
    src/core/providers/ProviderDiscovery.hpp
    src/core/providers/ProviderLauncher.cpp
    src/core/providers/ProviderLauncher.hpp

    src/fallback/prompt/TextNormalize.cpp
    src/fallback/prompt/TextNormalize.hpp
    src/fallback/prompt/PromptHeuristics.cpp
    src/fallback/prompt/PromptHeuristics.hpp
    src/fallback/prompt/PromptExtractors.cpp
    src/fallback/prompt/PromptExtractors.hpp
    src/fallback/prompt/PromptModel.hpp
    src/fallback/prompt/PromptModelBuilder.cpp
    src/fallback/prompt/PromptModelBuilder.hpp
    src/fallback/FallbackClient.cpp
    src/fallback/FallbackClient.hpp
    src/fallback/FallbackWindow.cpp
    src/fallback/FallbackWindow.hpp
)

target_link_libraries(bb-auth-tests
    PRIVATE
        Qt6::Test
        Qt6::Core
        Qt6::Widgets
        Qt6::Network
        Qt6::DBus
        PkgConfig::polkit_deps
)

add_test(NAME bb-auth-tests COMMAND bb-auth-tests)
set_tests_properties(bb-auth-tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Agent tooling tests: python3 drives the built binary and plugins as
# subprocesses (a check-time dependency only; nothing installed needs it).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME aisudo-shim
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_declare_shim.py)
    set_tests_properties(aisudo-shim PROPERTIES
        ENVIRONMENT "AISUDO=${CMAKE_BINARY_DIR}/aisudo")
    add_test(NAME bb-auth-intent-hook
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_intent_hook.py)
    set_tests_properties(bb-auth-intent-hook PROPERTIES
        ENVIRONMENT "BB_AUTH_INTENT_HOOK=${CMAKE_BINARY_DIR}/bb-auth-intent-hook")
    add_test(NAME bb-auth-pi-extension
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pi_extension.py)
    add_test(NAME bb-auth-opencode-plugin
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_opencode_plugin.py)
    add_test(NAME bb-auth-agents
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_agents_installer.py)
    set_tests_properties(bb-auth-agents PROPERTIES
        ENVIRONMENT "BB_AUTH_AGENTS=${CMAKE_BINARY_DIR}/bb-auth-agents")
endif()

install(PROGRAMS ${CMAKE_BINARY_DIR}/bb-auth-bootstrap
        DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})

# Install symlinks for protocol compatibility
install(CODE "
    if(\"${CMAKE_INSTALL_LIBEXECDIR}\" MATCHES \"^/\")
        set(_bb_auth_libexec_dir \"\$ENV{DESTDIR}${CMAKE_INSTALL_LIBEXECDIR}\")
    else()
        set(_bb_auth_libexec_dir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBEXECDIR}\")
    endif()

    execute_process(
        COMMAND \${CMAKE_COMMAND} -E create_symlink
            bb-auth
            bb-keyring-prompter
        WORKING_DIRECTORY \"\${_bb_auth_libexec_dir}\"
    )
    execute_process(
        COMMAND \${CMAKE_COMMAND} -E create_symlink
            bb-auth
            pinentry-bb
        WORKING_DIRECTORY \"\${_bb_auth_libexec_dir}\"
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
