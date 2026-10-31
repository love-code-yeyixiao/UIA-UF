cmake_minimum_required(VERSION 3.21)

project(UIA-UF VERSION 0.0.1 LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "UIA-UF only targets Windows.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # Static CRT: the service must not depend on a VC++ redistributable.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
    add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
endif()

# ---------------------------------------------------------------------------
# Shared headers (protocol, Win32 helpers). Header-only, no link step.
# ---------------------------------------------------------------------------
add_library(uiauf_common INTERFACE)
target_include_directories(uiauf_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Generates a VERSIONINFO resource from src/common/version.rc.in so the version
# stays in one place: project(UIA-UF VERSION ...).
function(uiauf_add_version_resource target description internal_name original_filename)
    set(UIAUF_FILE_DESCRIPTION "${description}")
    set(UIAUF_INTERNAL_NAME "${internal_name}")
    set(UIAUF_ORIGINAL_FILENAME "${original_filename}")
    set(rc_file "${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/common/version.rc.in" "${rc_file}" @ONLY)
    target_sources(${target} PRIVATE "${rc_file}")
endfunction()

# ---------------------------------------------------------------------------
# UIA-UF.Service.exe -- LocalSystem service that stamps TokenUIAccess onto the
# calling user's token and launches allow-listed applications.
# ---------------------------------------------------------------------------
add_executable(uiauf_service
    src/service/allowlist.cpp
    src/service/launcher.cpp
    src/service/lifecycle.cpp
    src/service/log.cpp
    src/service/main.cpp
    src/service/server.cpp
    src/service/service.cpp
)
set_target_properties(uiauf_service PROPERTIES OUTPUT_NAME "UIA-UF.Service")
target_link_libraries(uiauf_service PRIVATE uiauf_common advapi32 bcrypt user32 userenv wtsapi32)
uiauf_add_version_resource(uiauf_service "UIA-UF Service" "UIA-UF.Service" "UIA-UF.Service.exe")

# ---------------------------------------------------------------------------
# uiauf.exe -- client side launcher / inspector.
# ---------------------------------------------------------------------------
add_executable(uiauf src/cli/main.cpp src/cli/toast.cpp)
target_link_libraries(uiauf PRIVATE uiauf_common advapi32 bcrypt shell32 user32)
uiauf_add_version_resource(uiauf "UIA-UF" "uiauf" "uiauf.exe")

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT uiauf)

install(TARGETS uiauf_service uiauf RUNTIME DESTINATION bin)
