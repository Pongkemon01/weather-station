cmake_minimum_required(VERSION 3.21)

project(robin_wsc
    VERSION 1.0.0
    DESCRIPTION "Robin Weather Station Configurator"
    HOMEPAGE_URL "https://robinlab.ku.ac.th"
    LANGUAGES C CXX
)

#---------------------------------------------------------------------------
# Language standards
#---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

#---------------------------------------------------------------------------
# Qt setup
#---------------------------------------------------------------------------
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Search for AUTOUIC files in src/ subdirectories as well
set(CMAKE_AUTOUIC_SEARCH_PATHS
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dialogs
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tabs
)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Widgets SerialPort)

qt_standard_project_setup()

#---------------------------------------------------------------------------
# Warning hygiene — matches firmware-side standards
#---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wnull-dereference
        -Wdouble-promotion
    )
endif()

#---------------------------------------------------------------------------
# Sources
#---------------------------------------------------------------------------
set(APP_SOURCES
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/mainwindow.ui
    src/device_controller.cpp
    src/device_controller.h
    src/device_io.cpp
    src/device_io.h
    src/frame_parser.cpp
    src/frame_parser.h
    src/log_buffer.cpp
    src/log_buffer.h
    src/dialogs/device_picker_dialog.cpp
    src/dialogs/device_picker_dialog.h
    src/dialogs/device_picker_dialog.ui
    src/dialogs/log_viewer_dialog.cpp
    src/dialogs/log_viewer_dialog.h
    src/dialogs/log_viewer_dialog.ui
)

# Shared protocol code (compiled into both firmware and host)
# Currently header-only — the protocol has no CRC routines or other
# .c files. Add entries here if shared .c files are introduced later.
set(SHARED_SOURCES "")

set(APP_RESOURCES
    resources/robin_wsc.qrc
)

# Windows version-info resource
if(WIN32)
    list(APPEND APP_SOURCES resources/robin_wsc.rc)
endif()

#---------------------------------------------------------------------------
# Executable
#---------------------------------------------------------------------------
# WIN32 here suppresses the console window on Windows GUI apps
qt_add_executable(robin_wsc WIN32
    ${APP_SOURCES}
    ${SHARED_SOURCES}
    ${APP_RESOURCES}
)

target_include_directories(robin_wsc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared
)

target_link_libraries(robin_wsc PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::SerialPort
)

target_compile_definitions(robin_wsc PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
)

#---------------------------------------------------------------------------
# Static-link the MinGW C/C++ runtime so the .exe carries no GCC dependency
#---------------------------------------------------------------------------
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(robin_wsc PRIVATE
        -static-libgcc
        -static-libstdc++
        -static
    )
endif()

#---------------------------------------------------------------------------
# Install rules — produces a portable folder when invoked with
#   cmake --install build/release --prefix dist/RobinWSC-1.0.0-win64
#---------------------------------------------------------------------------
install(TARGETS robin_wsc RUNTIME DESTINATION .)

# License & readme alongside the binary (LGPLv3 compliance)
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/packaging/README.txt
    ${CMAKE_CURRENT_SOURCE_DIR}/packaging/COPYING.LGPLv3.txt
    DESTINATION .
)

# Auto-bundle Qt DLLs and plugins on Windows via windeployqt
if(WIN32)
    # Locate windeployqt next to the Qt installation
    get_target_property(_qmake_executable Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qmake_executable}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_bin_dir}")

    install(CODE "
        message(STATUS \"Running windeployqt on robin_wsc.exe ...\")
        execute_process(
            COMMAND \"${WINDEPLOYQT_EXECUTABLE}\"
                --release
                --no-translations
                --no-system-d3d-compiler
                --no-opengl-sw
                --no-quick-import
                --no-compiler-runtime
                --no-network
                --skip-plugin-types
                generic,iconengines,imageformats,networkinformation,tls
                \"\${CMAKE_INSTALL_PREFIX}/robin_wsc.exe\"
        )
    ")
endif()

#---------------------------------------------------------------------------
# Tests (optional — built only when BUILD_TESTING is ON)
#---------------------------------------------------------------------------
option(BUILD_TESTING "Build unit tests" OFF)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
