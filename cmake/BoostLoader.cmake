set(BOOST_VERSION 1.87.0)

set(BOOST_COMPONENTS
        log
        container
        locale
        process
        # Align needed by boost::json
        align
        json
        system
)
find_package(Boost ${BOOST_VERSION} COMPONENTS ${BOOST_COMPONENTS} QUIET)
if (NOT Boost_FOUND)
    message(STATUS "Boost (or some required components) not found, falling back to FetchContent instead")

    # Header-only libraries still need their real targets and transitive includes
    # in Boost's split source layout. Installed Boost already supplies these headers.
    # state/config.hpp uses UUID generators/io in public server headers.
    set(BOOST_INCLUDE_LIBRARIES ${BOOST_COMPONENTS} property_tree uuid)
    set(BOOST_ENABLE_CMAKE ON)
    # Simple-Web-Server consumes the official Boost::boost compatibility target.
    set(BOOST_ENABLE_COMPATIBILITY_TARGETS ON)
    FetchContent_Declare(
            Boost
            URL "https://github.com/boostorg/boost/releases/download/boost-${BOOST_VERSION}/boost-${BOOST_VERSION}-cmake.tar.xz"
            URL_HASH SHA256=7da75f171837577a52bbf217e17f8ea576c7c246e4594d617bfde7fafd408be5
    )
    FetchContent_MakeAvailable(Boost)

    set(Boost_FOUND TRUE)
    # Source-layout include paths come from the component targets, not a global
    # monolithic include directory (libs/headers/include is not such a tree).
    set(Boost_INCLUDE_DIRS "")
    set(Boost_LIBRARIES "")  # cmake-lint: disable=C0103
    foreach (component ${BOOST_COMPONENTS} property_tree uuid)
        list(APPEND Boost_LIBRARIES "Boost::${component}")
    endforeach ()
endif ()
include_directories(${Boost_INCLUDE_DIRS})
