# Simple-Web-Server 546895a93a29062bb178367b46c7afb72da9881e repeats
# find_package(Boost), which cannot discover an in-tree FetchContent build.
# Reuse the real compatibility target when provided; preserve standalone lookup.
set(cmake_file "${SIMPLEWEBSERVER_SOURCE_DIR}/CMakeLists.txt")
file(READ "${cmake_file}" content)

set(original [=[    find_package(Boost 1.53.0 COMPONENTS system)
    if(NOT Boost_FOUND)
        # Fallback: try without system component for newer Boost versions where it's header-only
        find_package(Boost 1.53.0 REQUIRED)
    endif()]=])
set(patched [=[    if(NOT TARGET Boost::boost)
        find_package(Boost 1.53.0 COMPONENTS system)
        if(NOT Boost_FOUND)
            # Fallback: try without system component for newer Boost versions where it's header-only
            find_package(Boost 1.53.0 REQUIRED)
        endif()
    endif()]=])

string(FIND "${content}" "${patched}" patched_position)
if(NOT patched_position EQUAL -1)
    return()
endif()
string(FIND "${content}" "${original}" original_position)
if(original_position EQUAL -1)
    message(FATAL_ERROR "Simple-Web-Server Boost lookup changed; review PatchSimpleWebBoost.cmake")
endif()
string(REPLACE "${original}" "${patched}" content "${content}")
file(WRITE "${cmake_file}" "${content}")
