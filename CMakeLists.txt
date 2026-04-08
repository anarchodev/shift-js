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
# qjs_snap: standalone QuickJS snapshot library
# ----------------------------------------------------------------------
add_library(qjs_snap STATIC src/qjs_snap.c)
target_include_directories(qjs_snap PUBLIC src)
target_link_libraries(qjs_snap PUBLIC qjs)

# ----------------------------------------------------------------------
# kvstore: KV storage library (SQLite-backed)
# ----------------------------------------------------------------------
add_library(kvstore STATIC src/kvstore.c)
target_include_directories(kvstore PUBLIC src)
target_link_libraries(kvstore PUBLIC sqlite3)

# ----------------------------------------------------------------------
# code_db: content-addressed code storage library
# ----------------------------------------------------------------------
add_library(code_db STATIC src/code_db.c)
target_include_directories(code_db PUBLIC src)
target_link_libraries(code_db PUBLIC sqlite3 OpenSSL::Crypto)

# ----------------------------------------------------------------------
# preprocessors: EJS + TypeScript (Sucrase) transform pipeline
# ----------------------------------------------------------------------
add_library(preprocessors STATIC src/preprocessor.c src/ejs.c src/typescript.c)
add_dependencies(preprocessors sucrase_bundle)
target_include_directories(preprocessors PUBLIC src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(preprocessors PUBLIC qjs)

# ----------------------------------------------------------------------
# log_db: per-worker request logging (SQLite + binary replay log)
# ----------------------------------------------------------------------
add_library(log_db STATIC src/log_db.c)
target_include_directories(log_db PUBLIC src)
target_link_libraries(log_db PUBLIC sqlite3)

# ----------------------------------------------------------------------
# replay_capture: deterministic replay tape recording
# ----------------------------------------------------------------------
add_library(replay_capture STATIC src/replay_capture.c)
target_include_directories(replay_capture PUBLIC src)
target_link_libraries(replay_capture PUBLIC kvstore)

# ----------------------------------------------------------------------
# sjs_raft: Raft consensus subsystem
# ----------------------------------------------------------------------
add_library(sjs_raft STATIC
    src/raft_thread.c
    src/raft_net.c
    src/raft_rpc.c
    src/raft_log.c
)
target_include_directories(sjs_raft PUBLIC src)
target_link_libraries(sjs_raft PUBLIC kvstore raft sqlite3 OpenSSL::Crypto uring pthread)

# ----------------------------------------------------------------------
# code_store: in-memory bytecode hash map
# ----------------------------------------------------------------------
add_library(code_store STATIC src/code_store.c)
target_include_directories(code_store PUBLIC src)
target_link_libraries(code_store PUBLIC pthread)

# ----------------------------------------------------------------------
# sjs: single executable with subcommands
# ----------------------------------------------------------------------
add_executable(sjs
    src/main.c
    src/worker.c
    src/admin.c
    src/ctl.c
    src/code_server.c
    src/code_client.c
    src/js_runtime.c
    src/js_globals.c
    src/js_dispatch.c
    src/router.c
    src/session.c
    src/crypto.c
)
target_include_directories(sjs PRIVATE src)
target_link_libraries(sjs PRIVATE
    shift_h2
    qjs_snap
    kvstore
    code_db
    code_store
    preprocessors
    log_db
    replay_capture
    sjs_raft
    OpenSSL::Crypto
    pthread
    m
    dl
)
