cmake_minimum_required(VERSION 3.22)
project(lumen VERSION 0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets DBus Network WaylandClient Sql Concurrent)
find_package(PkgConfig REQUIRED)
pkg_check_modules(XTEST REQUIRED IMPORTED_TARGET x11 xtst)
include(GNUInstallDirs)
find_package(KF6Config REQUIRED)
find_package(KF6GlobalAccel REQUIRED)
find_package(KF6KIO REQUIRED)
find_package(KF6Runner REQUIRED)
find_package(KF6XmlGui REQUIRED)
find_package(KF6Solid REQUIRED)
find_package(ZXing REQUIRED)
find_package(LayerShellQt REQUIRED)

# File previews come from the Quick Look project next to this one.
set(QUICKLOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../linux-quicklook" CACHE PATH "linux-quicklook source folder")
add_subdirectory("${QUICKLOOK_DIR}" quicklook EXCLUDE_FROM_ALL)

# Everything but main(), shared with the tests.
add_library(lumen-core STATIC
    src/source.h
    src/launcher.cpp
    src/arguments.cpp
    src/pastestack.cpp
    src/windowlayouts.cpp
    src/menusearch.cpp
    src/focusmode.cpp
    src/browserhistory.cpp
    src/calchistory.cpp
    src/theme.cpp
    src/packages.cpp
    src/appquit.cpp
    src/language.cpp
    src/network.cpp
    src/keepawake.cpp
    src/devices.cpp
    src/favicons.cpp
    src/aichat.cpp
    src/outputview.cpp
    src/scriptcommands.cpp
    src/resultview.cpp
    src/fuzzy.cpp
    src/usage.cpp
    src/library.cpp
    src/krunnersource.cpp
    src/builtinsource.cpp
    src/clipboardhistory.cpp
    src/filesource.cpp
    src/windowtiling.cpp
    src/settingsdialog.cpp
    src/paster.cpp
    src/answers.cpp
    src/answersource.cpp
    src/previewpane.cpp
    src/emojisource.cpp
    src/systemsource.cpp
    src/scriptsource.cpp
    src/windowsource.cpp
    src/noteswindow.cpp
    src/appsource.cpp
    src/expansion.cpp
    src/timeparse.cpp
    src/timekeeper.cpp
    src/timesource.cpp
    src/ics.cpp
    src/calendarsource.cpp
    src/weathersource.cpp
    src/transforms.cpp
    src/devsource.cpp
    src/projectsource.cpp
    src/desktopsource.cpp
    src/apphotkeys.cpp
    src/screentools.cpp
    src/monitorsource.cpp
    src/downloadsource.cpp
    src/style.cpp
)
qt_policy(SET QTP0006 NEW)
qt6_generate_wayland_protocol_client_sources(lumen-core FILES ${CMAKE_CURRENT_SOURCE_DIR}/protocols/fake-input.xml
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/plasma-window-management.xml)
target_include_directories(lumen-core PUBLIC src)
target_link_libraries(lumen-core PUBLIC
    Qt6::Widgets Qt6::DBus Qt6::Network Qt6::Sql Qt6::Concurrent
    KF6::ConfigCore KF6::GlobalAccel KF6::KIOGui KF6::KIOWidgets KF6::Runner KF6::XmlGui KF6::Solid ZXing::ZXing
    LayerShellQt::Interface quicklook-preview Qt6::WaylandClient PkgConfig::XTEST)

add_executable(lumen src/main.cpp)

# The snippet expansion helper (a system service; see helper/install-helper.sh).
pkg_check_modules(EXPAND_DEPS IMPORTED_TARGET libevdev xkbcommon)
if(EXPAND_DEPS_FOUND)
    add_library(keymatcher STATIC helper/keymatcher.cpp)
    target_include_directories(keymatcher PUBLIC helper)
    target_link_libraries(keymatcher PUBLIC PkgConfig::EXPAND_DEPS)
    add_executable(lumen-expand-helper helper/main.cpp)
    target_link_libraries(lumen-expand-helper PRIVATE keymatcher)
endif()
target_link_libraries(lumen PRIVATE lumen-core)

# Keyboard and search behaviour, run headless: ctest --test-dir build
find_package(Qt6 COMPONENTS Test)
if(Qt6Test_FOUND)
    enable_testing()
    add_executable(lumen-tests tests/launchertest.cpp)
    target_link_libraries(lumen-tests PRIVATE lumen-core Qt6::Test Qt6::Network)
    add_test(NAME lumen-tests COMMAND lumen-tests)
    set_tests_properties(lumen-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(answers-tests tests/answerstest.cpp)
    target_link_libraries(answers-tests PRIVATE lumen-core Qt6::Test)
    add_test(NAME answers-tests COMMAND answers-tests)
    add_executable(timeparse-tests tests/timeparsetest.cpp)
    target_link_libraries(timeparse-tests PRIVATE lumen-core Qt6::Test)
    add_test(NAME timeparse-tests COMMAND timeparse-tests)
    add_executable(ics-tests tests/icstest.cpp)
    target_link_libraries(ics-tests PRIVATE lumen-core Qt6::Test)
    add_test(NAME ics-tests COMMAND ics-tests)
    add_executable(script-tests tests/scripttest.cpp)
    target_link_libraries(script-tests PRIVATE lumen-core Qt6::Test)
    add_test(NAME script-tests COMMAND script-tests)
    set_tests_properties(script-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(TARGET lumen-expand-helper)
        add_executable(expand-helper-tests tests/expandhelpertest.cpp)
        target_link_libraries(expand-helper-tests PRIVATE Qt6::Test Qt6::Network)
        target_compile_definitions(expand-helper-tests PRIVATE HELPER_PATH="$<TARGET_FILE:lumen-expand-helper>")
        add_dependencies(expand-helper-tests lumen-expand-helper)
        add_test(NAME expand-helper-tests COMMAND expand-helper-tests)
    endif()
endif()

install(TARGETS lumen RUNTIME DESTINATION bin)
set(LUMEN_BINARY "${CMAKE_INSTALL_FULL_BINDIR}/lumen")
configure_file(data/io.github.lumen.desktop.in io.github.lumen.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/io.github.lumen.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
