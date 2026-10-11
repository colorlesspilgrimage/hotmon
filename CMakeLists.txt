cmake_minimum_required(VERSION 3.20)
project(hotmon VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CURSES_NEED_WIDE ON)
find_package(Curses REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(YYJSON REQUIRED IMPORTED_TARGET yyjson)

find_package(GTest REQUIRED)
enable_testing()
include(GoogleTest)

file(GLOB HOTMON_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")
list(REMOVE_ITEM HOTMON_CORE_SOURCES "${CMAKE_SOURCE_DIR}/src/main.cpp")

add_library(hotmon_core STATIC ${HOTMON_CORE_SOURCES})
target_include_directories(hotmon_core PUBLIC "${CMAKE_SOURCE_DIR}/src" ${CURSES_INCLUDE_DIRS})
target_link_libraries(hotmon_core PUBLIC PkgConfig::YYJSON ${CURSES_LIBRARIES})
target_compile_options(hotmon_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(CURSES_CFLAGS)
  target_compile_options(hotmon_core PUBLIC ${CURSES_CFLAGS})
endif()

add_executable(hotmon src/main.cpp)
target_link_libraries(hotmon PRIVATE hotmon_core)
target_compile_options(hotmon PRIVATE -Wall -Wextra -Wpedantic -Werror)

install(TARGETS hotmon RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

file(GLOB HOTMON_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/*.cpp")
add_executable(hotmon_tests ${HOTMON_TEST_SOURCES})
target_link_libraries(hotmon_tests PRIVATE hotmon_core GTest::gtest_main)
target_compile_options(hotmon_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(hotmon_tests PRIVATE HOTMON_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
gtest_discover_tests(hotmon_tests)
add_test(NAME Packaging.InstallLayout COMMAND bash ${CMAKE_SOURCE_DIR}/tests/packaging/check_install_layout.sh ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR})
add_test(NAME Packaging.MetadataVersionsMatch COMMAND bash ${CMAKE_SOURCE_DIR}/tests/packaging/check_metadata_versions.sh ${CMAKE_SOURCE_DIR})
add_test(NAME Packaging.ScriptsRejectBadInput COMMAND bash ${CMAKE_SOURCE_DIR}/tests/packaging/check_script_input_rules.sh ${CMAKE_SOURCE_DIR})
add_test(NAME Packaging.WorkflowRules COMMAND bash ${CMAKE_SOURCE_DIR}/tests/packaging/check_workflow_rules.sh ${CMAKE_SOURCE_DIR})
add_test(NAME Packaging.AurChecksumUser COMMAND bash ${CMAKE_SOURCE_DIR}/tests/packaging/check_aur_checksum_user.sh ${CMAKE_SOURCE_DIR})
add_test(NAME Packaging.PacmanRootBuild COMMAND bash ${CMAKE_SOURCE_DIR}/tests/packaging/check_pacman_root_build.sh ${CMAKE_SOURCE_DIR})
