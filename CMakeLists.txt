cmake_minimum_required(VERSION 3.25)

project(bearls VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "[INFO] no build type given, defaulting to Release")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(BEARC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../bear-lang/bearc"
    CACHE PATH "path to the bearc source directory (bear-lang/bearc)")

if(NOT EXISTS "${BEARC_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "[bearls] no bearc sources at BEARC_DIR='${BEARC_DIR}', "
        "configure with -DBEARC_DIR=/path/to/bear-lang/bearc")
endif()

# libbearc is built from source alongside the server so both always share one toolchain and one set
# of flags. SYSTEM keeps its (and LLVM's) headers out of our warnings, EXCLUDE_FROM_ALL skips the
# bearc executable.
set(BUILD_LIB ON CACHE BOOL "Build a static archive: libbearc" FORCE)
add_subdirectory(${BEARC_DIR} ${CMAKE_BINARY_DIR}/bearc EXCLUDE_FROM_ALL SYSTEM)

find_package(LLVM REQUIRED CONFIG)
llvm_map_components_to_libnames(BEARLS_LLVM_LIBS Core Support Option IRReader)

add_executable(bearls
    src/main.cpp
    src/json.cpp
    src/transport.cpp
    src/protocol.cpp
    src/text.cpp
    src/analysis.cpp
    src/project.cpp
    src/server.cpp
)

target_include_directories(bearls PRIVATE src)
# nlohmann/json (header-only, vendored), SYSTEM keeps it out of our warnings
target_include_directories(bearls SYSTEM PRIVATE third_party)
# ContextDatabase's public header reaches into bearc's private src/ tree and into LLVM
target_include_directories(bearls SYSTEM PRIVATE "${BEARC_DIR}/src" ${LLVM_INCLUDE_DIRS})
target_link_libraries(bearls PRIVATE libbearc ${BEARLS_LLVM_LIBS})
# on windows, libstdc++ keeps std::print's console support in its experimental library
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_libraries(bearls PRIVATE stdc++exp)
endif()
target_compile_definitions(bearls PRIVATE BEARLS_VERSION="${PROJECT_VERSION}")
target_compile_options(bearls PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wnon-virtual-dtor
    # constructor params named after the members they initialize are intentional
    $<$<CXX_COMPILER_ID:GNU>:-Wshadow=local> $<$<CXX_COMPILER_ID:Clang>:-Wshadow>)

include(CheckIPOSupported)
check_ipo_supported(RESULT bearls_ipo_supported OUTPUT bearls_ipo_error)
if(bearls_ipo_supported AND (CMAKE_BUILD_TYPE STREQUAL "Release"))
    message(STATUS "[INFO] bearls IPO / LTO enabled")
    set_property(TARGET bearls PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

install(TARGETS bearls RUNTIME DESTINATION bin)
