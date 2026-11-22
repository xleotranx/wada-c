cmake_minimum_required(VERSION 3.16)
project(wada C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The core: no third-party header, no UI. It must compile alone.
add_library(wada_core STATIC
    src/core/wada_err.c
    src/core/wada_id.c
    src/core/wada_log.c
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

# The shell: window, commands, palette, status, product host, settings, i18n,
# provider modules and boot. CUI lives here so the app and every example
# compile it once.
add_library(wada_shell STATIC
    src/shell/wada_app.c
    src/shell/wada_boot.c
    src/shell/wada_host.c
    src/shell/wada_i18n.c
    src/shell/wada_instance.c
    src/shell/wada_module.c
    src/shell/wada_settings.c
    src/shell/wada_settings_ui.c
    vendor/cui/cui.c
)
target_include_directories(wada_shell PUBLIC src/core src/shell vendor/cui)
target_link_libraries(wada_shell PUBLIC wada_core wada_sqlite)

if(WIN32)
    target_link_libraries(wada_shell PUBLIC user32 gdi32 opengl32 shell32 imm32 dwmapi comdlg32 ole32)
elseif(APPLE)
    enable_language(OBJC)
    set_source_files_properties(vendor/cui/cui.c PROPERTIES COMPILE_FLAGS "-x objective-c -fobjc-arc")
    target_link_libraries(wada_shell PUBLIC "-framework Cocoa" "-framework QuartzCore" "-framework OpenGL")
else()
    find_package(Threads REQUIRED)
    target_link_libraries(wada_shell PUBLIC X11 dl m Threads::Threads)
endif()

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

# The shipped app: a product table and one boot call.
add_executable(wada
    src/main.c
    src/apps/notes/notes.c
    src/apps/notes/notes_product.c
    src/apps/learning/cards.c
    src/apps/learning/learning_product.c
)
target_include_directories(wada PRIVATE src/apps/notes src/apps/learning)
target_link_libraries(wada PRIVATE wada_shell)
if(WIN32)
    set_target_properties(wada PROPERTIES WIN32_EXECUTABLE ON)
endif()

# Examples: each is a product plus a main that boots it.
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

# Tests.
add_executable(wada_tests tests/test_core.c)
target_include_directories(wada_tests PRIVATE src/core)
target_link_libraries(wada_tests PRIVATE wada_core)

add_executable(wada_store_tests tests/test_store.c)
target_include_directories(wada_store_tests PRIVATE src/core src/providers/sqlite vendor/sqlite)
target_link_libraries(wada_store_tests PRIVATE wada_sqlite)

add_executable(wada_notes_tests tests/test_notes.c src/apps/notes/notes.c)
target_include_directories(wada_notes_tests PRIVATE src/core src/apps/notes)
target_link_libraries(wada_notes_tests PRIVATE wada_sqlite)

add_executable(wada_cards_tests tests/test_cards.c src/apps/learning/cards.c)
target_include_directories(wada_cards_tests PRIVATE src/core src/apps/learning)
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

enable_testing()
add_test(NAME core COMMAND wada_tests)
add_test(NAME store COMMAND wada_store_tests)
add_test(NAME notes COMMAND wada_notes_tests)
add_test(NAME cards COMMAND wada_cards_tests)
add_test(NAME host COMMAND wada_host_tests)
add_test(NAME i18n COMMAND wada_i18n_tests)
add_test(NAME plugin COMMAND wada_plugin_tests $<TARGET_FILE:wada_store_plugin> $<TARGET_FILE:wada_bad_plugin>)
add_test(NAME instance COMMAND wada_instance_tests)
