cmake_minimum_required (VERSION 3.15)

# On Windows the default vcpkg triplet builds zstd as a DLL, and zarchive.exe then fails to start unless zstd.dll sits next to it.
# The tool is built with the static MSVC runtime, so default to vcpkg's matching static triplet to get a single self-contained exe.
# This has to happen before project(), because the vcpkg toolchain file is processed there.
# To override, pass -DVCPKG_TARGET_TRIPLET=<triplet>, set the VCPKG_DEFAULT_TRIPLET environment variable, or use -DBUILD_STATIC_TOOL=OFF.
if (CMAKE_HOST_WIN32 AND NOT DEFINED VCPKG_TARGET_TRIPLET AND NOT DEFINED ENV{VCPKG_DEFAULT_TRIPLET}
    AND (NOT DEFINED BUILD_STATIC_TOOL OR BUILD_STATIC_TOOL))
    if (CMAKE_GENERATOR_PLATFORM MATCHES "^[Aa][Rr][Mm]64$"
        OR (NOT CMAKE_GENERATOR_PLATFORM AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^[Aa][Rr][Mm]64$"))
        set(VCPKG_TARGET_TRIPLET "arm64-windows-static" CACHE STRING "vcpkg target triplet")
    elseif (CMAKE_GENERATOR_PLATFORM STREQUAL "Win32")
        set(VCPKG_TARGET_TRIPLET "x86-windows-static" CACHE STRING "vcpkg target triplet")
    else()
        set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "vcpkg target triplet")
    endif()
endif()

project("ZArchive"
    VERSION "0.1.3"
    DESCRIPTION "Library for creating and reading zstd-compressed file archives"
    HOMEPAGE_URL "https://github.com/Exzap/ZArchive"
)

if (WIN32)
    option(BUILD_STATIC_TOOL "Build the standalone executable statically" ON)
elseif(UNIX)
    option(BUILD_STATIC_TOOL "Build the standalone executable statically" OFF)
    if (BUILD_STATIC_TOOL)
        set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++")
    endif()
endif()

if (BUILD_STATIC_TOOL)
    message(STATUS "Building standalone executable statically")
    set(VCPKG_LIBRARY_LINKAGE "static" CACHE STRING "Vcpkg target triplet")
    if (MSVC)
        # "-static" is a GCC/Clang option. MSVC's linker does not know it (warning LNK4044), static linking there comes from the
        # static runtime (MSVC_RUNTIME_LIBRARY below) and the static vcpkg triplet instead.
        set(STATIC_TOOL_FLAG "")
    else()
        set(STATIC_TOOL_FLAG "-static")
    endif()
else()
    message(STATUS "Building standalone executable dynamically")
    set(VCPKG_LIBRARY_LINKAGE "dynamic" CACHE STRING "Vcpkg target triplet")
    set(STATIC_TOOL_FLAG "")
endif()


# an unoptimized build is extremely slow for a compression tool, default to Release if the user did not choose
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)

set(SOURCE_FILES_LIB
    src/zarchivewriter.cpp
    src/zarchivereader.cpp
    src/sha_256.c
)

# build static library
add_library (zarchive ${SOURCE_FILES_LIB})
add_library (ZArchive::zarchive ALIAS zarchive)
target_compile_features(zarchive PUBLIC cxx_std_20)
set_target_properties(zarchive PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
)

target_include_directories(zarchive
    PUBLIC
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

find_package(zstd MODULE REQUIRED) # MODULE because zstd::zstd is not defined upstream
target_link_libraries(zarchive PUBLIC Threads::Threads PRIVATE zstd::zstd ${STATIC_TOOL_FLAG})

# standalone executable
add_executable (zarchiveTool src/main.cpp)
set_property(TARGET zarchiveTool PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(zarchiveTool PROPERTIES OUTPUT_NAME "zarchive")
target_link_libraries(zarchiveTool PRIVATE zarchive ${STATIC_TOOL_FLAG})

# install
install(DIRECTORY include/zarchive/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/zarchive" FILES_MATCHING PATTERN "zarchive*.h")
install(TARGETS zarchive)
install(TARGETS zarchiveTool)

# pkg-config
include(JoinPaths) # can be replaced by cmake_path(APPEND) in CMake 3.20
join_paths(PKGCONFIG_INCLUDEDIR "\${prefix}" "${CMAKE_INSTALL_INCLUDEDIR}")
join_paths(PKGCONFIG_LIBDIR "\${prefix}" "${CMAKE_INSTALL_LIBDIR}")

configure_file("zarchive.pc.in" "zarchive.pc" @ONLY)
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/zarchive.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
)
