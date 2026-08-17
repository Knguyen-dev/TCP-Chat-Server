cmake_minimum_required(VERSION 3.6...4.4)
project(
    TCPChatServer
    VERSION 0.1.0
    DESCRIPTION "A TCP chat server and client"
    LANGUAGES CXX
)

# -----------------------------
# Global Project Defaults
# -----------------------------

# Add cmake directory to search path and include the 
# Warnings file, which loads the set_project_warnings function.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(Warnings)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_program(IWYU_PATH NAMES "include-what-you-use" "iwyu")
if (IWYU_PATH)
    message(STATUS "Found Include-What-You-Use: ${IWYU_PATH}")

    # Define the IWYU command. Passing '-Xiwyu' and '--error' causes IWYU
    # to exit with code 1 if issues are found, which turns IWYU warnings into
    # hard build failures.
    # NOTE: Don't want to set IWYU globally because it will analyze google benchmark
    # files as well, causing unnecessary build time and clutter. Instead we'll
    # have IWYU be applied on specific build targets (our own code).
    set(IWYU_COMMAND "${IWYU_PATH}" "-Xiwyu" "--error")
else()
    message(WARNING "include-what-you-use not found on system PATH. IWYU analysis disabled.")
endif()

option(ENABLE_ASAN "Enable AddressSanitizer (ASan)" OFF)

# -----------------------------
# Helper Function for target configuration
# -----------------------------
function(configure_project_target TARGET_NAME)
    # Apply compiler warnings
    set_project_warnings(${TARGET_NAME})

    # Enable IWYU specifically for this target (if found)
    if (IWYU_PATH)
        set_target_properties(${TARGET_NAME} PROPERTIES
            CXX_INCLUDE_WHAT_YOU_USE "${IWYU_COMMAND}"
        )
    endif()

    # Enable ASan if flag was passed
    if(ENABLE_ASAN)
        target_compile_options(${TARGET_NAME} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${TARGET_NAME} PUBLIC -fsanitize=address)
    endif()
endfunction()


# -----------------------------
# Dependencies via FetchContent
# -----------------------------
include(FetchContent)

FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.8.3
)

# a. Don't run any of google benchmark's internal tests
# b. Don't copy benchmark's binaries, libs, and headers into system paths when installing
#    We don't plan to install it, so CMake will skip generating installation targets
# c. Disable building and running unit tests 
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_USE_BUNDLED_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)


# Try system-installed sqlite 3 first
find_package(SQLite3 QUIET)
if (SQLite3_FOUND)
    message(STATUS "Found system SQLite3 version: ${SQLite3_VERSION}")
    # User can now link against target: SQLite::SQLite3
else()
    # Otherwise, downlaod it
    message(STATUS "SQLite3 not found locally. Downloading amalgamation...")
    FetchContent_Declare(
        sqlite3
        URL https://sqlite.org/2026/sqlite-amalgamation-3530400.zip
    )
    FetchContent_Populate(sqlite3)
    
    # a. GLOB: Tells CMAake to search the directory tree for files matching the pattern.
    # b. SQLITE3_C_FILE: Variable name where CMake will store the matching path.
    # c. SQLITE3_DIR: Variable name containing the path to the source directory of sqlite3.
    # Since the downloaded zip extracts into a variable folder name like sqlite-amalgamation-3530400/
    # we have to use a wildcard to automatically match that folder.
    file(GLOB SQLITE3_C_FILE "${sqlite3_SOURCE_DIR}/*/sqlite3.c")
    file(GLOB SQLITE3_DIR    "${sqlite3_SOURCE_DIR}/*/")

    # We'll create a static library with that C file, linking it in C, 
    # including sqlite3's include directory and compiling/linking the library.
    add_library(SQLite::SQLite3 STATIC ${SQLITE3_C_FILE})
    set_target_properties(SQLite::SQLite3 PROPERTIES LINKER_LANGUAGE C)
    target_include_directories(SQLite::SQLite3 INTERFACE ${SQLITE3_DIR})
    target_link_libraries(SQLite::SQLite3 PRIVATE ${CMAKE_DL_LIBS})
endif()


# -----------------------------
# Shared Core Libraries Target
# -----------------------------
add_library(
    chat_core
    src/shared.cpp
    src/logger.cpp
    src/protocol.cpp
)
target_include_directories(chat_core PUBLIC include)
configure_project_target(chat_core)

# -----------------------------
# Executable Targets
# -----------------------------
# Client executable target
add_executable(
    ${PROJECT_NAME}_client
    src/client.cpp
    src/client_utils.cpp
)

# NOTE: Inherits include directories from chat_core
# since chat_core is set to PUBLIC include.
target_link_libraries(
    ${PROJECT_NAME}_client
    PRIVATE
    chat_core
)
configure_project_target(${PROJECT_NAME}_client)

# NOTE: Listing all remaining source files that are 
# server specific. The shared.cpp, logger.cpp, 
# and protocol.cpp are already linked via chat_core.
# Also note the include directories are inherited from chat_core.
add_executable(
    ${PROJECT_NAME}_server
    src/server.cpp
    src/db.cpp
    src/server_utils.cpp
)

target_link_libraries(
    ${PROJECT_NAME}_server
    PRIVATE
    chat_core
    SQLite::SQLite3
)
configure_project_target(${PROJECT_NAME}_server)

# -----------------------------
# Benchmarks and Tests
# -----------------------------
add_executable(
    ${PROJECT_NAME}_benchmark
    test/benchmark/benchmark_test.cpp
)

# NOTE: No need to include the 'include' directories
# Since the benchmark file only uses system headers.
target_link_libraries(
    ${PROJECT_NAME}_benchmark
    PRIVATE
    benchmark::benchmark
)
