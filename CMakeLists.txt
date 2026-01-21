cmake_minimum_required(VERSION 3.16)

# Get version
file(READ "${CMAKE_SOURCE_DIR}/VERSION" VER_RAW)
string(STRIP ${VER_RAW} VER)

project(noctalia-auth VERSION ${VER} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network DBus)
find_package(PkgConfig REQUIRED)

if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Werror=format-security)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_definitions(_FORTIFY_SOURCE=2)
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

qt_standard_project_setup(REQUIRES 6.5)

# Unified noctalia-auth binary
qt_add_executable(noctalia-auth
    # Main entry point
    src/main.cpp

    # Common utilities
    src/common/Paths.cpp
    src/common/Paths.hpp

    # Core components
    src/core/Agent.cpp
    src/core/Agent.hpp
    src/core/PolkitListener.hpp
    src/core/PolkitListener.cpp
    src/core/RequestContext.hpp
    src/core/RequestContext.cpp

    # Mode handlers
    src/modes/daemon.cpp
    src/modes/daemon.hpp
    src/modes/keyring.cpp
    src/modes/keyring.hpp
    src/modes/pinentry.cpp
    src/modes/pinentry.hpp

    # Keyring prompter (C code, compiled into main binary)
    src/keyring-prompter/main.c
    src/keyring-prompter/noctalia-prompt.c
    src/keyring-prompter/noctalia-prompt.h
    src/keyring-prompter/ipc-client.c
    src/keyring-prompter/ipc-client.h
)

target_link_libraries(noctalia-auth
    PRIVATE
        Qt6::Core
        Qt6::Network
        Qt6::DBus
        PkgConfig::polkit_deps
        PkgConfig::gcr_deps
)

include(GNUInstallDirs)

set(LIBEXECDIR ${CMAKE_INSTALL_FULL_LIBEXECDIR})
set(DATADIR ${CMAKE_INSTALL_FULL_DATADIR})

# Configure service files
configure_file(assets/noctalia-auth.service.in noctalia-auth.service @ONLY)
configure_file(assets/noctalia-auth-dbus.service.in noctalia-auth-dbus.service @ONLY)
configure_file(assets/org.gnome.keyring.SystemPrompter.service.in org.gnome.keyring.SystemPrompter.service @ONLY)

# Install main binary
install(TARGETS noctalia-auth
        DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})

# Install symlinks for protocol compatibility
install(CODE "
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            noctalia-auth
            \$ENV{DESTDIR}${CMAKE_INSTALL_FULL_LIBEXECDIR}/noctalia-keyring-prompter
    )
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            noctalia-auth
            \$ENV{DESTDIR}${CMAKE_INSTALL_FULL_LIBEXECDIR}/pinentry-noctalia
    )
")

# Install systemd user service
install(FILES ${CMAKE_BINARY_DIR}/noctalia-auth.service
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/user")

# Install D-Bus service file
install(FILES ${CMAKE_BINARY_DIR}/noctalia-auth-dbus.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services
        RENAME org.noctalia.polkitagent.service)

# Install keyring prompter D-Bus service file template
install(FILES ${CMAKE_BINARY_DIR}/org.gnome.keyring.SystemPrompter.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/noctalia-auth)
