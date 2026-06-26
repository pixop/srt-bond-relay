cmake_minimum_required(VERSION 3.16)

project(srt-bond-relay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
    cpp_httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.48.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(cpp_httplib)
FetchContent_MakeAvailable(nlohmann_json)

find_package(SRT QUIET)

set(SRT_LINKAGE "dynamic" CACHE STRING "SRT linkage mode: dynamic or static")
set_property(CACHE SRT_LINKAGE PROPERTY STRINGS dynamic static)
if (NOT SRT_LINKAGE STREQUAL "dynamic" AND NOT SRT_LINKAGE STREQUAL "static")
    message(FATAL_ERROR "SRT_LINKAGE must be one of: dynamic, static")
endif()

set(PIXOP_SRT_LINK_TARGET "")
set(PIXOP_SRT_INCLUDE_DIRS "")
set(PIXOP_SRT_LIBRARY_DIRS "")
set(PIXOP_SRT_LIBRARIES "")
set(PIXOP_SRT_LINK_OPTIONS "")

if (SRT_LINKAGE STREQUAL "dynamic" AND SRT_FOUND AND TARGET SRT::srt)
    set(PIXOP_SRT_LINK_TARGET "SRT::srt")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SRT REQUIRED IMPORTED_TARGET srt)

    set(PIXOP_SRT_INCLUDE_DIRS "${SRT_INCLUDE_DIRS}")
    set(PIXOP_SRT_LIBRARY_DIRS "${SRT_LIBRARY_DIRS}")

    if (SRT_LINKAGE STREQUAL "dynamic")
        set(PIXOP_SRT_LINK_TARGET "PkgConfig::SRT")
        set(PIXOP_SRT_LIBRARIES "${SRT_LIBRARIES}")
    else()
        set(PKG_CONFIG_USE_STATIC_LIBS ON)
        pkg_check_modules(SRT_STATIC REQUIRED srt)
        set(PKG_CONFIG_USE_STATIC_LIBS OFF)

        set(_pixop_srt_static_candidates)
        foreach(_dir IN LISTS SRT_LIBRARY_DIRS SRT_STATIC_LIBRARY_DIRS)
            list(APPEND _pixop_srt_static_candidates
                "${_dir}/libsrt.a"
            )
        endforeach()
        list(APPEND _pixop_srt_static_candidates
            "/opt/pixop-srt/lib/libsrt.a"
            "/opt/pixop-srt/lib64/libsrt.a"
        )

        set(_pixop_srt_static_lib "")
        foreach(_candidate IN LISTS _pixop_srt_static_candidates)
            if (EXISTS "${_candidate}")
                set(_pixop_srt_static_lib "${_candidate}")
                break()
            endif()
        endforeach()
        if (_pixop_srt_static_lib STREQUAL "")
            message(FATAL_ERROR "SRT_LINKAGE=static requested but libsrt.a was not found")
        endif()

        set(PIXOP_SRT_LINK_TARGET "${_pixop_srt_static_lib}")
        set(PIXOP_SRT_LIBRARIES "${SRT_STATIC_LIBRARIES}")
        list(REMOVE_ITEM PIXOP_SRT_LIBRARIES srt)
        set(PIXOP_SRT_LINK_OPTIONS "${SRT_STATIC_LDFLAGS_OTHER}")
    endif()
endif()

add_executable(srt-bond-relay
    main.cpp
    src/config.cpp
    src/linkage.cpp
    src/logger.cpp
    src/metrics.cpp
    src/relay_endpoint_specs.cpp
    src/relay_input_sources.cpp
    src/relay_output_sinks.cpp
    src/relay_srt_endpoints.cpp
    src/relay_runtime.cpp
    src/srt_utils.cpp
)

target_compile_definitions(srt-bond-relay PRIVATE SRT_LINKAGE_MODE="${SRT_LINKAGE}")

target_compile_options(srt-bond-relay PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

if (PIXOP_SRT_INCLUDE_DIRS)
    target_include_directories(srt-bond-relay PRIVATE ${PIXOP_SRT_INCLUDE_DIRS})
endif()
target_include_directories(srt-bond-relay PRIVATE
    include
)
if (PIXOP_SRT_LIBRARY_DIRS)
    target_link_directories(srt-bond-relay PRIVATE ${PIXOP_SRT_LIBRARY_DIRS})
endif()

target_link_libraries(srt-bond-relay PRIVATE
    ${PIXOP_SRT_LINK_TARGET}
    ${PIXOP_SRT_LIBRARIES}
    httplib::httplib
    nlohmann_json::nlohmann_json
    dl
)
if (PIXOP_SRT_LINK_OPTIONS)
    target_link_options(srt-bond-relay PRIVATE ${PIXOP_SRT_LINK_OPTIONS})
endif()

# Keep runtime linkage pinned to the custom libsrt directory in the image.
set_target_properties(srt-bond-relay PROPERTIES
    BUILD_RPATH "/opt/pixop-srt/lib"
    INSTALL_RPATH "/opt/pixop-srt/lib"
)

install(TARGETS srt-bond-relay
    RUNTIME DESTINATION bin
)
