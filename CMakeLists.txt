cmake_minimum_required(VERSION 3.16)

# Get version
file(READ "${CMAKE_SOURCE_DIR}/VERSION" VER_RAW)
string(STRIP ${VER_RAW} VER)

project(hpa VERSION ${VER} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network DBus)
find_package(PkgConfig REQUIRED)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Werror=format-security)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_definitions(_FORTIFY_SOURCE=2)
    endif()
    add_link_options(-Wl,-z,relro -Wl,-z,now)
endif()

pkg_check_modules(
  deps
  REQUIRED
  IMPORTED_TARGET
  polkit-agent-1
  polkit-qt6-1)

# Dependencies for keyring-prompter
pkg_check_modules(
  gcr_deps
  REQUIRED
  IMPORTED_TARGET
  gcr-4
  gio-2.0
  json-glib-1.0)

qt_standard_project_setup(REQUIRES 6.5)

# Main polkit agent
qt_add_executable(noctaliapolkitagent
    src/main.cpp
    src/core/Agent.cpp
    src/core/Agent.hpp
    src/core/PolkitListener.hpp
    src/core/PolkitListener.cpp
    src/core/RequestContext.hpp
    src/core/RequestContext.cpp
)

target_link_libraries(noctaliapolkitagent
    PRIVATE Qt6::Core Qt6::Network Qt6::DBus PkgConfig::deps
)

# Keyring prompter (C executable)
add_executable(noctalia-keyring-prompter
    src/keyring-prompter/main.c
    src/keyring-prompter/noctalia-prompt.c
    src/keyring-prompter/noctalia-prompt.h
    src/keyring-prompter/ipc-client.c
    src/keyring-prompter/ipc-client.h
)

target_link_libraries(noctalia-keyring-prompter
    PRIVATE PkgConfig::gcr_deps
)

include(GNUInstallDirs)

set_target_properties(noctaliapolkitagent PROPERTIES OUTPUT_NAME "noctalia-polkit")

set(LIBEXECDIR ${CMAKE_INSTALL_FULL_LIBEXECDIR})
set(DATADIR ${CMAKE_INSTALL_FULL_DATADIR})
configure_file(assets/noctalia-polkit.service.in noctalia-polkit.service @ONLY)
configure_file(assets/noctalia-polkit-dbus.service.in noctalia-polkit-dbus.service @ONLY)
configure_file(assets/org.gnome.keyring.SystemPrompter.service.in org.gnome.keyring.SystemPrompter.service @ONLY)

install(TARGETS noctaliapolkitagent noctalia-keyring-prompter
        DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
install(FILES ${CMAKE_BINARY_DIR}/noctalia-polkit.service
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/user")
install(FILES ${CMAKE_BINARY_DIR}/noctalia-polkit-dbus.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services
        RENAME org.noctalia.polkitagent.service)
install(FILES ${CMAKE_BINARY_DIR}/org.gnome.keyring.SystemPrompter.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/noctalia-polkit)
