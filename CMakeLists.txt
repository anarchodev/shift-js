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
    GIT_TAG        71a3c54ff3555f56cf2a56e1774e25b7a2b7ff5b
    PATCH_COMMAND   git checkout -- . && git apply ${CMAKE_SOURCE_DIR}/patches/quickjs-stable-shape-hash.patch
)

FetchContent_Declare(sqlite3
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3450000.zip
)

FetchContent_Declare(dmon
    GIT_REPOSITORY https://github.com/septag/dmon.git
    GIT_TAG        master
)

FetchContent_Declare(raft
    GIT_REPOSITORY https://github.com/willemt/raft.git
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

# -- raft: Raft consensus library (willemt/raft) -------------------------
FetchContent_GetProperties(raft)
if(NOT raft_POPULATED)
    FetchContent_Populate(raft)
endif()

add_library(raft STATIC
    ${raft_SOURCE_DIR}/src/raft_server.c
    ${raft_SOURCE_DIR}/src/raft_server_properties.c
    ${raft_SOURCE_DIR}/src/raft_node.c
    ${raft_SOURCE_DIR}/src/raft_log.c
)
target_include_directories(raft PUBLIC ${raft_SOURCE_DIR}/include)
target_compile_options(raft PRIVATE -w)

# -- Sucrase: TypeScript transpiler (JS bundle embedded via QuickJS) ---
set(SUCRASE_JS "${CMAKE_SOURCE_DIR}/vendor/sucrase-bundle.js")
set(SUCRASE_HEADER "${CMAKE_BINARY_DIR}/generated/sucrase_bundle.h")

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")

add_custom_command(
    OUTPUT "${SUCRASE_HEADER}"
    COMMAND xxd -i sucrase-bundle.js "${SUCRASE_HEADER}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/vendor"
    DEPENDS "${SUCRASE_JS}"
    COMMENT "Generating embedded Sucrase bundle header"
)

add_custom_target(sucrase_bundle DEPENDS "${SUCRASE_HEADER}")

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
    src/typescript.c
    src/session.c
    src/crypto.c
    src/raft_log.c
    src/raft_rpc.c
    src/raft_net.c
    src/raft_thread.c
    src/log_db.c
    src/replay_capture.c
)
add_dependencies(shift-js sucrase_bundle)
target_include_directories(shift-js PRIVATE src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(shift-js PRIVATE
    shift_h2
    qjs
    sqlite3
    raft
    OpenSSL::Crypto
    pthread
    uring
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
target_link_libraries(sjsctl PRIVATE sqlite3 pthread OpenSSL::Crypto)
