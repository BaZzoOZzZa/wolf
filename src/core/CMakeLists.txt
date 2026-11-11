add_library(wolf_core)
add_library(wolf::core ALIAS wolf_core)

target_include_directories(wolf_core PUBLIC src)

# Platform agnostic code
add_subdirectory(src/platforms/all/helpers)
target_link_libraries(wolf_core PUBLIC wolf::helpers)

add_subdirectory(src/platforms/all/docker)
target_link_libraries(wolf_core PUBLIC wolf::docker)

file(GLOB CORE_HEADERS CONFIGURE_DEPENDS src/core/*.hpp)
set(CORE_SRC "")

FetchContent_Declare(
        immer
        GIT_REPOSITORY https://github.com/arximboldi/immer.git
        GIT_TAG e02cbd795e9424a8405a8cb01f659ad61c0cbbc7)
set(immer_BUILD_TESTS OFF)
set(immer_BUILD_EXAMPLES OFF)
set(immer_BUILD_DOCS OFF)
set(immer_BUILD_EXTRAS OFF)

set(FPHSA_NAME_MISMATCHED on) # see: https://github.com/arximboldi/immer/issues/204
FetchContent_MakeAvailable(immer)
target_link_libraries_system(wolf_core PUBLIC immer)
unset(FPHSA_NAME_MISMATCHED)

find_package(PkgConfig)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-base-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
pkg_check_modules(GLIB2 REQUIRED IMPORTED_TARGET glib-2.0)

if (NOT (GSTREAMER_FOUND))
    message(FATAL_ERROR "Please Install Gstreamer Dev: CMake will Exit")
endif ()
target_link_libraries(wolf_core PUBLIC
        PkgConfig::GSTREAMER
        PkgConfig::GLIB2)

# Platform dependent code
if (UNIX AND NOT APPLE)
    add_subdirectory(src/platforms/linux/pulseaudio)
    target_link_libraries(wolf_core PUBLIC wolf::audio)

    list(APPEND CORE_SRC src/platforms/linux/batched_send/batched_send.cpp)

    option(WOLF_CUSTOM_INPUTTINO_SRC "Use custom inputtino source" OFF)
    set(INPUTTINO_MOTION_PATCH_DRIVER "${PROJECT_SOURCE_DIR}/cmake/PatchInputtinoMotion.cmake")
    function(wolf_verify_inputtino_motion source_dir)
        execute_process(COMMAND "${CMAKE_COMMAND}" "-DINPUTTINO_SOURCE_DIR=${source_dir}"
                        -DINPUTTINO_VERIFY_ONLY=ON -P "${INPUTTINO_MOTION_PATCH_DRIVER}"
                        RESULT_VARIABLE motion_check_result)
        if (NOT motion_check_result EQUAL 0)
            message(FATAL_ERROR "Inputtino signed-motion verification failed")
        endif ()
    endfunction()
    if (WOLF_CUSTOM_INPUTTINO_SRC)
        wolf_verify_inputtino_motion("${WOLF_CUSTOM_INPUTTINO_SRC}")
        add_subdirectory(${WOLF_CUSTOM_INPUTTINO_SRC} ${CMAKE_CURRENT_BINARY_DIR}/inputtino EXCLUDE_FROM_ALL)
    else ()
        FetchContent_Declare(
                inputtino
                GIT_REPOSITORY https://github.com/games-on-whales/inputtino.git
                GIT_TAG fd136cf
                PATCH_COMMAND "${CMAKE_COMMAND}" "-DINPUTTINO_SOURCE_DIR=<SOURCE_DIR>"
                              -P "${INPUTTINO_MOTION_PATCH_DRIVER}")
        # FETCHCONTENT_SOURCE_DIR_INPUTTINO skips the FetchContent patch step.
        # Fail closed instead of silently modifying a caller-owned source tree.
        if (FETCHCONTENT_SOURCE_DIR_INPUTTINO)
            wolf_verify_inputtino_motion("${FETCHCONTENT_SOURCE_DIR_INPUTTINO}")
        endif ()
        FetchContent_MakeAvailable(inputtino)
        wolf_verify_inputtino_motion("${inputtino_SOURCE_DIR}")
    endif ()

    add_subdirectory(src/platforms/linux/uinput)
    target_link_libraries(wolf_core PUBLIC wolf::uinput)

    message(STATUS "Using Gstreamer bindings for Wayland")
    list(APPEND CORE_SRC src/platforms/linux/virtual-display/gst-wayland-display.cpp)
else ()
    file(GLOB CORE_SRC SRCS platforms/unknown/*.cpp)
endif ()

target_sources(wolf_core PUBLIC ${CORE_HEADERS} PRIVATE ${CORE_SRC})

# All users of this library will need at least C++17
target_compile_features(wolf_core PUBLIC cxx_std_17)
set_target_properties(wolf_core PROPERTIES LINKER_LANGUAGE CXX)
