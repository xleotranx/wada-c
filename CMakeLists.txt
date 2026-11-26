cmake_minimum_required(VERSION 3.16)
project(wada C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(WADA_IS_TOPLEVEL ON)
else()
    set(WADA_IS_TOPLEVEL OFF)
endif()
option(WADA_BUILD_APPS "Build Wada apps and examples" ${WADA_IS_TOPLEVEL})
option(WADA_BUILD_TESTS "Build Wada tests" ${WADA_IS_TOPLEVEL})

# The core: no third-party header, no UI. It must compile alone.
add_library(wada_core STATIC
    src/core/wada_err.c
    src/core/wada_id.c
    src/core/wada_log.c
    src/core/wada_records.c
    src/core/wada_scope.c
    src/core/wada_event.c
    src/core/wada_registry.c
)
target_include_directories(wada_core PUBLIC src/core)
if(MSVC)
    target_compile_options(wada_core PRIVATE /W4)
else()
    target_compile_options(wada_core PRIVATE -Wall -Wextra)
endif()

# SQLite as a vendored provider: the core never includes sqlite3.h.
add_library(wada_sqlite STATIC
    src/providers/sqlite/wada_store_sqlite.c
    vendor/sqlite/sqlite3.c
)
target_include_directories(wada_sqlite PUBLIC src/core src/providers/sqlite vendor/sqlite)
target_compile_definitions(wada_sqlite PRIVATE
    SQLITE_ENABLE_FTS5
    SQLITE_DQS=0
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_DEPRECATED
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
)
target_link_libraries(wada_sqlite PUBLIC wada_core)

# The runtime: everything an app needs when it brings its own window shell —
# core, the SQLite provider, i18n, the single-instance lock and the vendored
# CUI toolkit. A native-shell app (Wada Snap) links this instead of wada_shell,
# so the CUI application loop is not compiled into it.
add_library(wada_runtime STATIC
    src/shell/wada_i18n.c
    src/shell/wada_instance.c
    vendor/cui/cui.c
)
target_include_directories(wada_runtime PUBLIC src/core src/shell vendor/cui)
target_link_libraries(wada_runtime PUBLIC wada_core wada_sqlite)

if(WIN32)
    target_link_libraries(wada_runtime PUBLIC user32 gdi32 opengl32 shell32 imm32 dwmapi comdlg32 ole32)
elseif(APPLE)
    enable_language(OBJC)
    set_source_files_properties(vendor/cui/cui.c PROPERTIES COMPILE_FLAGS "-x objective-c -fobjc-arc -D_DARWIN_C_SOURCE")
    target_link_libraries(wada_runtime PUBLIC "-framework Cocoa" "-framework QuartzCore" "-framework OpenGL")
else()
    find_package(Threads REQUIRED)
    target_link_libraries(wada_runtime PUBLIC X11 dl m Threads::Threads)
endif()

# The shell: the CUI window, commands, palette, status, product host, settings,
# provider modules and boot. It sits on top of the runtime.
add_library(wada_shell STATIC
    src/shell/wada_app.c
    src/shell/wada_boot.c
    src/shell/wada_host.c
    src/shell/wada_module.c
    src/shell/wada_settings.c
    src/shell/wada_settings_ui.c
    src/shell/wada_ui.c
)
target_include_directories(wada_shell PUBLIC src/core src/shell vendor/cui)
target_link_libraries(wada_shell PUBLIC wada_runtime)

if(WADA_BUILD_APPS OR WADA_BUILD_TESTS)
# The same SQLite provider delivered as a loadable module. The executable can
# start with --provider <this file> and never link the provider at all.
add_library(wada_store_plugin MODULE
    src/providers/sqlite/wada_store_plugin.c
    src/providers/sqlite/wada_store_sqlite.c
    vendor/sqlite/sqlite3.c
)
target_include_directories(wada_store_plugin PRIVATE src/core src/providers/sqlite vendor/sqlite)
target_compile_definitions(wada_store_plugin PRIVATE
    SQLITE_ENABLE_FTS5
    SQLITE_DQS=0
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_DEPRECATED
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
)
target_link_libraries(wada_store_plugin PRIVATE wada_core)
set_target_properties(wada_store_plugin PROPERTIES PREFIX "")

# A provider that lies about its ABI: the loader must refuse it.
add_library(wada_bad_plugin MODULE tests/bad_plugin.c)
target_include_directories(wada_bad_plugin PRIVATE src/core)
set_target_properties(wada_bad_plugin PROPERTIES PREFIX "")
endif()

if(WADA_BUILD_APPS)
# The shipped app: a product table and one boot call.
add_executable(wada
    src/main.c
    examples/notes/notes.c
    examples/notes/notes_product.c
    examples/learning/cards.c
    examples/learning/learning_product.c
)
target_include_directories(wada PRIVATE examples/notes examples/learning)
target_link_libraries(wada PRIVATE wada_shell)
if(WIN32)
    set_target_properties(wada PROPERTIES WIN32_EXECUTABLE ON)
endif()

# Examples: each is a product plus a main that boots it.
add_executable(wada_notes
    examples/notes/main.c
    examples/notes/notes.c
    examples/notes/notes_product.c
)
target_include_directories(wada_notes PRIVATE examples/notes)
target_link_libraries(wada_notes PRIVATE wada_shell)
if(WIN32)
    set_target_properties(wada_notes PROPERTIES WIN32_EXECUTABLE ON)
endif()

add_executable(wada_learning
    examples/learning/main.c
    examples/learning/cards.c
    examples/learning/learning_product.c
)
target_include_directories(wada_learning PRIVATE examples/learning)
target_link_libraries(wada_learning PRIVATE wada_shell)
if(WIN32)
    set_target_properties(wada_learning PROPERTIES WIN32_EXECUTABLE ON)
endif()

add_executable(wada_hello
    examples/hello/main.c
    examples/hello/hello_product.c
)
target_link_libraries(wada_hello PRIVATE wada_shell)
if(WIN32)
    set_target_properties(wada_hello PROPERTIES WIN32_EXECUTABLE ON)
endif()

add_executable(wada_todo
    examples/todo/main.c
    examples/todo/todo_product.c
)
target_link_libraries(wada_todo PRIVATE wada_shell)
if(WIN32)
    set_target_properties(wada_todo PROPERTIES WIN32_EXECUTABLE ON)
endif()

add_executable(wada_bookmarks
    examples/bookmarks/main.c
    examples/bookmarks/bookmarks_product.c
)
target_link_libraries(wada_bookmarks PRIVATE wada_shell)
if(WIN32)
    set_target_properties(wada_bookmarks PROPERTIES WIN32_EXECUTABLE ON)
endif()

add_executable(wada_workbench
    examples/workbench/main.c
    examples/workbench/wb_store.c
    examples/workbench/wb_issues_product.c
    examples/workbench/wb_activity_product.c
)
target_include_directories(wada_workbench PRIVATE examples/workbench)
target_link_libraries(wada_workbench PRIVATE wada_shell)
if(WIN32)
    set_target_properties(wada_workbench PROPERTIES WIN32_EXECUTABLE ON)
endif()
endif() # WADA_BUILD_APPS

if(WADA_BUILD_TESTS)
# Tests.
add_executable(wada_tests tests/test_core.c)
target_include_directories(wada_tests PRIVATE src/core)
target_link_libraries(wada_tests PRIVATE wada_core)

add_executable(wada_records_tests tests/test_records.c)
target_include_directories(wada_records_tests PRIVATE src/core src/providers/sqlite)
target_link_libraries(wada_records_tests PRIVATE wada_sqlite)

add_executable(wada_store_tests tests/test_store.c)
target_include_directories(wada_store_tests PRIVATE src/core src/providers/sqlite vendor/sqlite)
target_link_libraries(wada_store_tests PRIVATE wada_sqlite)

add_executable(wada_notes_tests tests/test_notes.c examples/notes/notes.c)
target_include_directories(wada_notes_tests PRIVATE src/core examples/notes)
target_link_libraries(wada_notes_tests PRIVATE wada_sqlite)

add_executable(wada_cards_tests tests/test_cards.c examples/learning/cards.c)
target_include_directories(wada_cards_tests PRIVATE src/core examples/learning)
target_link_libraries(wada_cards_tests PRIVATE wada_sqlite)

add_executable(wada_host_tests tests/test_host.c)
target_link_libraries(wada_host_tests PRIVATE wada_shell)

add_executable(wada_i18n_tests tests/test_i18n.c)
target_link_libraries(wada_i18n_tests PRIVATE wada_shell)

add_executable(wada_plugin_tests tests/test_plugin.c)
target_link_libraries(wada_plugin_tests PRIVATE wada_shell)
add_dependencies(wada_plugin_tests wada_store_plugin wada_bad_plugin)

add_executable(wada_instance_tests tests/test_instance.c)
target_link_libraries(wada_instance_tests PRIVATE wada_shell)

add_executable(wada_workbench_tests tests/test_workbench.c examples/workbench/wb_store.c)
target_include_directories(wada_workbench_tests PRIVATE examples/workbench)
target_link_libraries(wada_workbench_tests PRIVATE wada_shell)

enable_testing()
add_test(NAME core COMMAND wada_tests)
add_test(NAME records COMMAND wada_records_tests)
add_test(NAME store COMMAND wada_store_tests)
add_test(NAME notes COMMAND wada_notes_tests)
add_test(NAME cards COMMAND wada_cards_tests)
add_test(NAME host COMMAND wada_host_tests)
add_test(NAME i18n COMMAND wada_i18n_tests)
add_test(NAME plugin COMMAND wada_plugin_tests $<TARGET_FILE:wada_store_plugin> $<TARGET_FILE:wada_bad_plugin>)
add_test(NAME instance COMMAND wada_instance_tests)
add_test(NAME workbench COMMAND wada_workbench_tests)
endif() # WADA_BUILD_TESTS
