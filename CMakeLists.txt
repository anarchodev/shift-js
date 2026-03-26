cmake_minimum_required(VERSION 3.24)
project(shift_js VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ----------------------------------------------------------------------
# Dependencies
# ----------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(shift
    GIT_REPOSITORY https://github.com/anarchodev/shift.git
    GIT_TAG        main
)

FetchContent_Declare(shift_h2
    GIT_REPOSITORY https://github.com/anarchodev/shift-h2.git
    GIT_TAG        main
)

FetchContent_Declare(quickjs
    GIT_REPOSITORY https://github.com/quickjs-ng/quickjs.git
    GIT_TAG        master
)

FetchContent_Declare(sqlite3
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3450000.zip
)

FetchContent_Declare(dmon
    GIT_REPOSITORY https://github.com/septag/dmon.git
    GIT_TAG        master
)

# -- shift and shift-h2 have their own CMakeLists ---
set(ENABLE_TLS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(shift shift_h2)

# -- quickjs-ng: use its own CMakeLists (provides 'qjs' target) --------
# Disable quickjs extras we don't need
set(BUILD_QJS_LIBC OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(quickjs)

# -- sqlite3: build amalgamation as static library ----------------------
FetchContent_GetProperties(sqlite3)
if(NOT sqlite3_POPULATED)
    FetchContent_Populate(sqlite3)
endif()

add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${sqlite3_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=2
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_DQS=0
)
target_compile_options(sqlite3 PRIVATE -w)

# -- dmon: header-only file watcher ----------------------------------------
FetchContent_GetProperties(dmon)
if(NOT dmon_POPULATED)
    FetchContent_Populate(dmon)
endif()

# -- OpenSSL: crypto primitives for Web Crypto API ---------------------
find_package(OpenSSL REQUIRED)

# ----------------------------------------------------------------------
# shift-js server
# ----------------------------------------------------------------------
add_executable(shift-js
    src/main.c
    src/worker.c
    src/kvstore.c
    src/js_runtime.c
    src/router.c
    src/preprocessor.c
    src/ejs.c
    src/session.c
    src/crypto.c
)
target_include_directories(shift-js PRIVATE src)
target_link_libraries(shift-js PRIVATE
    shift_h2
    qjs
    sqlite3
    OpenSSL::Crypto
    pthread
    m
    dl
)

# ----------------------------------------------------------------------
# CLI tool
# ----------------------------------------------------------------------
add_executable(sjsctl
    tools/sjsctl.c
    src/kvstore.c
)
target_include_directories(sjsctl PRIVATE src ${dmon_SOURCE_DIR})
target_link_libraries(sjsctl PRIVATE sqlite3 pthread)
