cmake_minimum_required(VERSION 3.21)
project(VAAPIChecker VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
include(GNUInstallDirs)
include(CTest)
if(BUILD_TESTING AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_checker.cpp")
    message(WARNING "tests/test_checker.cpp is missing; building the application without QtTest.")
    set(BUILD_TESTING OFF)
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Concurrent DBus)
find_package(PkgConfig REQUIRED)
pkg_check_modules(VA REQUIRED IMPORTED_TARGET libva>=1.12 libva-drm>=1.12)

add_library(vaapi_core STATIC
    src/core/report.cpp
    src/core/va_probe.cpp
    src/core/gpu_monitor.cpp
    src/core/media_tools.cpp
)
target_include_directories(vaapi_core PUBLIC src)
target_link_libraries(vaapi_core PUBLIC Qt6::Core PkgConfig::VA)
target_compile_options(vaapi_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(vaapi_ui STATIC
    src/ui/main_window.cpp
    src/ui/main_window.ui
    src/ui/capability_tab.ui
    src/ui/processing_tab.ui
    src/ui/media_tab.ui
    src/ui/trace_tab.ui
    src/ui/usage_tab.ui
    src/ui/theme_controller.cpp
)
target_link_libraries(vaapi_ui PUBLIC vaapi_core Qt6::Widgets Qt6::Concurrent Qt6::DBus)
target_compile_options(vaapi_ui PRIVATE -Wall -Wextra -Wpedantic)

add_executable(vaapi-checker src/main.cpp)
target_link_libraries(vaapi-checker PRIVATE vaapi_ui)
target_compile_definitions(vaapi-checker PRIVATE APP_VERSION="${PROJECT_VERSION}")
qt_add_resources(vaapi-checker "icons" PREFIX "/icons" FILES packaging/vaapi-checker.svg)

if(BUILD_TESTING)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
    add_executable(vaapi-checker-tests tests/test_checker.cpp)
    target_link_libraries(vaapi-checker-tests PRIVATE vaapi_ui Qt6::Test)
    qt_add_resources(vaapi-checker-tests "test_icons" PREFIX "/icons" FILES packaging/vaapi-checker.svg)
    add_test(NAME checker COMMAND vaapi-checker-tests)
    set_tests_properties(checker PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

install(TARGETS vaapi-checker RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/io.github.vaapichecker.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/vaapi-checker.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
