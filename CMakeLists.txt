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
FetchContent_MakeAvailable(cpp_httplib)

find_package(SRT QUIET)
set(PIXOP_SRT_LINK_TARGET "")
set(PIXOP_SRT_INCLUDE_DIRS "")
set(PIXOP_SRT_LIBRARY_DIRS "")
set(PIXOP_SRT_LIBRARIES "")

if (SRT_FOUND AND TARGET SRT::srt)
    set(PIXOP_SRT_LINK_TARGET "SRT::srt")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SRT REQUIRED IMPORTED_TARGET srt)
    set(PIXOP_SRT_LINK_TARGET "PkgConfig::SRT")
    set(PIXOP_SRT_INCLUDE_DIRS "${SRT_INCLUDE_DIRS}")
    set(PIXOP_SRT_LIBRARY_DIRS "${SRT_LIBRARY_DIRS}")
    set(PIXOP_SRT_LIBRARIES "${SRT_LIBRARIES}")
endif()

add_executable(srt-bond-relay
    main.cpp
)

target_compile_options(srt-bond-relay PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

if (PIXOP_SRT_INCLUDE_DIRS)
    target_include_directories(srt-bond-relay PRIVATE ${PIXOP_SRT_INCLUDE_DIRS})
endif()
if (PIXOP_SRT_LIBRARY_DIRS)
    target_link_directories(srt-bond-relay PRIVATE ${PIXOP_SRT_LIBRARY_DIRS})
endif()

target_link_libraries(srt-bond-relay PRIVATE
    ${PIXOP_SRT_LINK_TARGET}
    ${PIXOP_SRT_LIBRARIES}
    httplib::httplib
    dl
)

# Keep runtime linkage pinned to the custom libsrt directory in the image.
set_target_properties(srt-bond-relay PROPERTIES
    BUILD_RPATH "/opt/pixop-srt/lib"
    INSTALL_RPATH "/opt/pixop-srt/lib"
)

install(TARGETS srt-bond-relay
    RUNTIME DESTINATION bin
)
