cmake_minimum_required(VERSION 3.16...3.24)

# Testing library
FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.8.0
)

FetchContent_MakeAvailable(Catch2)

project(wolftests CXX C)

# Tests need to be added as executables first
add_executable(wolftests main.cpp)

set(SRC_LIST
        main.cpp
        testControl.cpp
        testCrypto.cpp
        testGSTPlugin.cpp
        testMoonlight.cpp
        testRTSP.cpp
        testWolfAPI.cpp
        testBatchedSend.cpp)

option(TEST_RUST_WAYLAND "Test our custom Rust wayland compositor" ON)
if (UNIX AND NOT APPLE)
    if (TEST_RUST_WAYLAND)
        find_package(PkgConfig)
        pkg_check_modules(wayland-client REQUIRED IMPORTED_TARGET wayland-client)

        # Add the wayland protocols
        find_package(WaylandScanner REQUIRED QUIET)
        pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
        pkg_get_variable(WAYLAND_PROTOCOLS_PKGDATADIR wayland-protocols pkgdatadir)
        # xdg-shell
        ecm_add_wayland_client_protocol(wolftests
                PROTOCOL ${WAYLAND_PROTOCOLS_PKGDATADIR}/stable/xdg-shell/xdg-shell.xml
                BASENAME xdg-shell)
        # relative-pointer
        ecm_add_wayland_client_protocol(wolftests
                PROTOCOL ${WAYLAND_PROTOCOLS_PKGDATADIR}/unstable/relative-pointer/relative-pointer-unstable-v1.xml
                BASENAME relative-pointer)

        # Add the generated headers and files
        include_directories(${CMAKE_CURRENT_BINARY_DIR})

        list(APPEND SRC_LIST
                "platforms/linux/wayland-client.hpp"
                "platforms/linux/wayland-display.hpp"
                "platforms/linux/wayland-display.cpp"
        )
        target_link_libraries(wolftests PRIVATE PkgConfig::wayland-client)
    endif ()

    option(TEST_NVIDIA "Enable Nvidia tests" ON)
    if (TEST_NVIDIA)
        list(APPEND SRC_LIST "platforms/linux/nvidia.cpp")
    endif ()

    option(TEST_VIRTUAL_INPUT "Enable libinput test" ON)
    if (TEST_VIRTUAL_INPUT)
        find_package(PkgConfig)
        pkg_check_modules(LIBINPUT REQUIRED IMPORTED_TARGET libinput)
        target_link_libraries(wolftests PRIVATE PkgConfig::LIBINPUT)

        option(TEST_UHID "Enable uhid test" ON)
        if (TEST_UHID)
            list(APPEND SRC_LIST "platforms/linux/uhid.cpp")
        endif ()

        if (BUILD_FAKE_UDEV_CLI)
            list(APPEND SRC_LIST "platforms/linux/fake-udev.cpp")
            target_link_libraries(wolftests PRIVATE fake-udev::lib)
        endif ()
        list(APPEND SRC_LIST
                "platforms/linux/libinput.h"
                "platforms/linux/inputtino.cpp")
    endif ()
endif ()

find_package(CURL)
target_link_libraries(wolftests PUBLIC CURL::libcurl)

list(APPEND SRC_LIST "testSerialization.cpp")

option(TEST_DOCKER "Enable docker tests" ON)
if (TEST_DOCKER)
    list(APPEND SRC_LIST "docker/testDocker.cpp")
endif ()

option(TEST_EXCEPTIONS "Enable exceptions tests" ON)
if (TEST_EXCEPTIONS)
    list(APPEND SRC_LIST testExceptions.cpp)
endif ()

target_sources(wolftests PRIVATE ${SRC_LIST})

# I'm using C++17 in the test
target_compile_features(wolftests PRIVATE cxx_std_17)

# Should be linked to the main library, as well as the Catch2 testing library
target_link_libraries_system(wolftests PRIVATE
        wolf::runner
        Catch2::Catch2)

# Load test-only modules from the same pinned split Boost without adding them
# to Boost::headers/Boost_LIBRARIES and thereby exposing Beast to production.
function(wolf_link_beast_for_tests)
    FetchContent_GetProperties(Boost SOURCE_DIR boost_test_source BINARY_DIR boost_test_binary)
    if (boost_test_source)
        set(BOOST_SUPERPROJECT_VERSION "${BOOST_VERSION}")
        set(BUILD_TESTING OFF) # Do not enable the dependencies' own test suites.
        # Missing header-only dependencies of Beast in the pinned Boost 1.87.
        foreach (component logic static_string beast)
            if (NOT TARGET Boost::${component})
                add_subdirectory("${boost_test_source}/libs/${component}"
                        "${boost_test_binary}/wolf-test-boost/${component}" EXCLUDE_FROM_ALL)
            endif ()
        endforeach ()
    endif ()
    # Installed Boost keeps its existing common header path; use its component
    # target too when the package exposes one.
    if (TARGET Boost::beast)
        target_link_libraries(wolftests PRIVATE Boost::beast)
    endif ()
endfunction()
wolf_link_beast_for_tests()

## Test assets
configure_file(assets/config.test.toml ${CMAKE_CURRENT_BINARY_DIR}/config.test.toml COPYONLY)
configure_file(assets/config.test.toml ${CMAKE_CURRENT_BINARY_DIR}/config.test.EDITED.toml COPYONLY)

# See: https://github.com/catchorg/Catch2/blob/devel/docs/cmake-integration.md
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)
option(WOLF_RUNTIME_TEST_DISCOVERY "Register upstream tests whose main initializes streaming" ON)
if (WOLF_RUNTIME_TEST_DISCOVERY)
    # Even --list-tests runs the upstream main and initializes streaming. Keep
    # ordinary CI registration, but never execute that main as a build step.
    catch_discover_tests(wolftests
            DISCOVERY_MODE PRE_TEST
            PROPERTIES
            ENVIRONMENT "WOLF_SKIP_WAYLAND_SOCKET_WAIT=TRUE")
endif ()

# Isolated suites use Catch2's main and the real production libraries. Neither
# executable includes main.cpp or initializes streaming for test discovery.
add_executable(wolf-protocol-security-tests testCrypto.cpp testControl.cpp)
target_link_libraries(wolf-protocol-security-tests PRIVATE wolf::moonlight wolf::helpers Catch2::Catch2WithMain)
target_include_directories(wolf-protocol-security-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/moonlight-server"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/src")
if (TARGET inputtino::libinputtino)
    # control.hpp uses inputtino's public device/input types. Avoid linking all
    # of wolf::core (which also propagates PulseAudio implementation sources).
    target_link_libraries(wolf-protocol-security-tests PRIVATE inputtino::libinputtino)
endif ()
target_compile_features(wolf-protocol-security-tests PRIVATE cxx_std_20)
add_test(NAME wolf-protocol-security-tests COMMAND wolf-protocol-security-tests)

add_executable(wolf-api-security-tests testWolfAPI.cpp)
target_link_libraries(wolf-api-security-tests PRIVATE wolf::runner Catch2::Catch2WithMain CURL::libcurl)
target_compile_features(wolf-api-security-tests PRIVATE cxx_std_20)
# Other tests in testWolfAPI.cpp require the upstream environment. Only the
# security selector is safe here; fail rather than silently passing zero cases.
add_test(NAME wolf-api-security-tests COMMAND wolf-api-security-tests "[security]")
set_tests_properties(wolf-protocol-security-tests wolf-api-security-tests PROPERTIES LABELS "security;isolated")

# State-only regression executable: no Wolf runtime, Docker daemon or devices.
find_package(Threads REQUIRED)
add_executable(wolf-lifecycle-tests testLifecycle.cpp)
target_compile_features(wolf-lifecycle-tests PRIVATE cxx_std_20)
target_include_directories(wolf-lifecycle-tests PRIVATE ../src/moonlight-server ../src/core/src)
target_link_libraries(wolf-lifecycle-tests PRIVATE Threads::Threads)
add_test(NAME wolf-lifecycle-tests COMMAND wolf-lifecycle-tests)

if (UNIX AND NOT APPLE)
    add_executable(wolf-docker-lifecycle-tests docker/testDockerLifecycle.cpp)
    target_compile_features(wolf-docker-lifecycle-tests PRIVATE cxx_std_20)
    target_include_directories(wolf-docker-lifecycle-tests PRIVATE ../src/core/src ../src/moonlight-server)
    target_link_libraries(wolf-docker-lifecycle-tests PRIVATE wolf::docker Threads::Threads)
    add_test(NAME wolf-docker-lifecycle-tests COMMAND wolf-docker-lifecycle-tests)
    set_tests_properties(wolf-docker-lifecycle-tests PROPERTIES TIMEOUT 30)
endif ()

# Exercise the actual patched inputtino encoder without opening input devices
# or entering Wolf's GStreamer-initialising test main.
if (TARGET inputtino::libinputtino)
    get_target_property(INPUTTINO_CHECKED_SOURCE_DIR libinputtino SOURCE_DIR)
    add_executable(wolf-inputtino-motion-tests testInputtinoSignedMotion.cpp)
    target_compile_features(wolf-inputtino-motion-tests PRIVATE cxx_std_17)
    target_include_directories(wolf-inputtino-motion-tests PRIVATE "${INPUTTINO_CHECKED_SOURCE_DIR}/src/uhid")
    target_link_libraries(wolf-inputtino-motion-tests PRIVATE Catch2::Catch2WithMain)
    add_test(NAME inputtino-signed-motion COMMAND wolf-inputtino-motion-tests)
endif ()

# Scope foundation: real event-bus/lifecycle adapters, no runtime/device startup.
add_executable(wolf-connection-scope-tests testConnectionScopes.cpp)
target_link_libraries(wolf-connection-scope-tests PRIVATE wolf::runner Catch2::Catch2WithMain)
target_compile_features(wolf-connection-scope-tests PRIVATE cxx_std_20)
add_test(NAME wolf-connection-scope-tests COMMAND wolf-connection-scope-tests)
set_tests_properties(wolf-connection-scope-tests PROPERTIES LABELS "scope;isolated" TIMEOUT 30)

# Consumer ownership: real registry/event adapters and private-context software
# Gst only. No upstream main, Wolf streaming init, device or network listener.
add_executable(wolf-consumer-runtime-tests testConsumerRuntime.cpp)
target_link_libraries(wolf-consumer-runtime-tests PRIVATE wolf::runner Catch2::Catch2WithMain)
target_compile_features(wolf-consumer-runtime-tests PRIVATE cxx_std_20)
add_test(NAME wolf-consumer-runtime-tests COMMAND wolf-consumer-runtime-tests)
set_tests_properties(wolf-consumer-runtime-tests PROPERTIES LABELS "consumer;isolated" TIMEOUT 45)
