cmake_minimum_required(VERSION 3.19)
project(AScript VERSION 1.0.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Compiler / standard
# ---------------------------------------------------------------------------
if(NOT CMAKE_CXX_COMPILER)
    find_program(GXX g++)
    if(GXX)
        set(CMAKE_CXX_COMPILER ${GXX})
    endif()
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type - use empty to avoid adding default flags
set(CMAKE_BUILD_TYPE "" CACHE STRING "" FORCE)

# Optimization level
set(ASCRIPT_OPT_LEVEL "2" CACHE STRING "Optimization level (0/1/2/3)")
set_property(CACHE ASCRIPT_OPT_LEVEL PROPERTY STRINGS 0 1 2 3)
add_compile_options(-O${ASCRIPT_OPT_LEVEL})

# Split debug info
option(ASCRIPT_SPLIT_DEBUG "Separate debug info into .debug files" OFF)
if(ASCRIPT_SPLIT_DEBUG)
    add_compile_options(-g)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# AlxLib paths
# ---------------------------------------------------------------------------
set(ALXLIB_DIR ${CMAKE_SOURCE_DIR}/lib/alxlib)
set(ALXLIB_INC ${ALXLIB_DIR}/include)
set(ALXLIB_LIB ${ALXLIB_DIR}/lib)
set(ALXLIB_RUNTIME ${CMAKE_SOURCE_DIR}/bin/alxlib)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(BUILD_CLI "Build axc-cli" ON)
option(BUILD_GUI "Build axc-gui" ON)
option(BUILD_LINKS "Build link libraries" OFF)
option(CLI_STATIC "Build axc-cli with static linking" OFF)

# ---------------------------------------------------------------------------
# axc-core
# ---------------------------------------------------------------------------
# Generate res.cpp
file(GLOB RES_JSON_FILES ${CMAKE_SOURCE_DIR}/res/core/*.json ${CMAKE_SOURCE_DIR}/res/cli/*.json)
file(GLOB RES_MD_FILES ${CMAKE_SOURCE_DIR}/res/core/*.md)
add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/src/core/res.cpp
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_res.py
            ${CMAKE_SOURCE_DIR}/res
            core
            ${CMAKE_SOURCE_DIR}/src/core/res.cpp
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_res.py
            ${CMAKE_SOURCE_DIR}/tools/md2js.py
            ${RES_JSON_FILES}
            ${RES_MD_FILES}
    COMMENT "Generating res.cpp from JSON/markdown resources"
)

add_library(axc-core SHARED
    # facade
    src/core/core.cpp
    src/core/extend.cpp
    src/core/inspect.cpp
    src/core/project.cpp
    # generated
    src/core/res.cpp
    # extends
    src/core/impl/global.cpp
    src/core/impl/io.cpp
    src/core/impl/sys.cpp
    src/core/impl/math.cpp
    src/core/impl/vec.cpp
    src/core/impl/lst.cpp
    src/core/impl/map.cpp
    src/core/impl/str.cpp
    src/core/impl/regx.cpp
    src/core/impl/bytes.cpp
    src/core/impl/hash.cpp
    src/core/impl/kv.cpp
    src/core/impl/thd.cpp
    src/core/impl/fs.cpp
    src/core/impl/evt.cpp
)

target_include_directories(axc-core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${ALXLIB_INC}/alxbase
        ${ALXLIB_INC}/alxcore
        ${ALXLIB_INC}/alxscpt
)

target_link_libraries(axc-core
    PUBLIC
        ${ALXLIB_RUNTIME}/libalxscpt.so
        ${ALXLIB_RUNTIME}/libalxcore.so
        ${ALXLIB_RUNTIME}/libalxbase.so
)

set_target_properties(axc-core PROPERTIES
    SOVERSION 2 # ax_config layout changed (name/out) — forces a loud relink
    VERSION 2.0.0
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

# ---------------------------------------------------------------------------
# axc-cli (no Qt dependency)
# ---------------------------------------------------------------------------
if(BUILD_CLI)
    set(CLI_SOURCES
        src/cli/main.cpp
        src/cli/cli.cpp
        src/cli/editor.cpp
    )

    if(CLI_STATIC)
        list(APPEND CLI_SOURCES
            src/core/core.cpp
            src/core/extend.cpp
            src/core/inspect.cpp
            src/core/project.cpp
            src/core/res.cpp
            src/core/impl/global.cpp
            src/core/impl/io.cpp
            src/core/impl/sys.cpp
            src/core/impl/math.cpp
            src/core/impl/vec.cpp
            src/core/impl/lst.cpp
            src/core/impl/map.cpp
            src/core/impl/str.cpp
            src/core/impl/regx.cpp
            src/core/impl/bytes.cpp
            src/core/impl/hash.cpp
            src/core/impl/kv.cpp
            src/core/impl/thd.cpp
            src/core/impl/fs.cpp
            src/core/impl/evt.cpp
        )
    endif()

    add_executable(axc-cli ${CLI_SOURCES})

    target_include_directories(axc-cli
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/cli
            ${CMAKE_CURRENT_SOURCE_DIR}/src/core
            ${ALXLIB_INC}/alxbase
            ${ALXLIB_INC}/alxcore
            ${ALXLIB_INC}/alxscpt
    )

    if(CLI_STATIC)
        target_link_libraries(axc-cli PRIVATE ${ALXLIB_LIB}/libalxscpt.a)
        set_target_properties(axc-cli PROPERTIES
            BUILD_RPATH ""
            INSTALL_RPATH ""
        )
    else()
        target_link_libraries(axc-cli PRIVATE axc-core)
        set_target_properties(axc-cli PROPERTIES
            BUILD_RPATH "$ORIGIN;$ORIGIN/alexis"
            INSTALL_RPATH "$ORIGIN;$ORIGIN/alexis"
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# axc-runner (no Qt) — one run per process, driven by the GUI over comm_ex
# ---------------------------------------------------------------------------
option(BUILD_RUNNER "Build axc-runner" ON)
if(BUILD_RUNNER)
    add_executable(axc-runner
        src/runner/main.cpp
        src/runner/control.cpp
        src/runner/hook.cpp
        src/runner/output.cpp
        src/runner/session.cpp
        src/runner/snapshot.cpp
        src/runner/source_registry.cpp
        src/runner/watchdog.cpp
        src/proto/message.cpp
    )

    target_include_directories(axc-runner
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${ALXLIB_INC}/alxcomm
    )

    target_link_libraries(axc-runner PRIVATE axc-core ${ALXLIB_RUNTIME}/libalxcomm.so)

    set_target_properties(axc-runner PROPERTIES
        BUILD_RPATH "$ORIGIN;$ORIGIN/alxlib"
        INSTALL_RPATH "$ORIGIN;$ORIGIN/alxlib"
    )
endif()

# ---------------------------------------------------------------------------
# axc-replay (no Qt) — headless GUI stand-in for the runner protocol tests
# ---------------------------------------------------------------------------
option(BUILD_REPLAY "Build the axc-replay protocol driver" OFF)
if(BUILD_REPLAY)
    add_executable(axc-replay
        test/runner/replay.cpp
        src/proto/message.cpp
    )

    target_include_directories(axc-replay
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${ALXLIB_INC}/alxcomm
    )

    target_link_libraries(axc-replay PRIVATE axc-core ${ALXLIB_RUNTIME}/libalxcomm.so)

    set_target_properties(axc-replay PROPERTIES
        BUILD_RPATH "$ORIGIN;$ORIGIN/alxlib"
        INSTALL_RPATH "$ORIGIN;$ORIGIN/alxlib"
    )
endif()

# ---------------------------------------------------------------------------
# axc-gui (Qt6)
# ---------------------------------------------------------------------------
if(BUILD_GUI)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Svg)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTOUIC ON)
    set(CMAKE_AUTORCC ON)

    add_executable(axc-gui
        src/gui/main.cpp
        src/gui/gui.cpp
        src/gui/QScriptMain.cpp
        src/gui/CompilePanel.cpp
        src/gui/LangRefDock.cpp
        src/gui/OpenFilesPanel.cpp
        src/gui/WelcomePage.cpp
        src/gui/ScriptEditor.cpp
        src/gui/EditorArea.cpp
        src/gui/AsHighlighter.cpp
        src/gui/CompletionPopup.cpp
        src/gui/HintProvider.cpp
        src/gui/FileTreeDock.cpp
        src/gui/PropsPanel.cpp
        src/gui/EmbedWidget.cpp
        src/gui/UiTerminal.cpp
        src/gui/EngineObject.cpp
        src/gui/ViewDock.cpp
        src/gui/PerfChart.cpp
        src/gui/BreakpointStore.cpp
        src/gui/debug/RunnerProcess.cpp
        src/gui/debug/Session.cpp
        src/gui/debug/VariablesModel.cpp
        src/gui/debug/SessionManager.cpp
        src/gui/debug/PerfSampler.cpp
        src/proto/message.cpp
        src/gui/QScriptSetting.cpp
        src/gui/QScriptProject.cpp
        src/gui/Settings.cpp
        res/gui/AScript.qrc
    )

    target_include_directories(axc-gui
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/gui
            ${CMAKE_CURRENT_SOURCE_DIR}/src/core
            ${ALXLIB_INC}/alxcomm
    )

    target_link_libraries(axc-gui PRIVATE
        axc-core
        ${ALXLIB_RUNTIME}/libalxcomm.so
        Qt6::Core
        Qt6::Widgets
        Qt6::Svg
    )

    set_target_properties(axc-gui PROPERTIES
        BUILD_RPATH "$ORIGIN;$ORIGIN/alxlib"
        INSTALL_RPATH "$ORIGIN;$ORIGIN/alxlib"
    )
endif()

# ---------------------------------------------------------------------------
# axc-gui-test — Qt-core test for the session/runner layer (no widgets)
# ---------------------------------------------------------------------------
option(BUILD_GUI_TESTS "Build the Qt-core session test" OFF)
if(BUILD_GUI_TESTS)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui)

    add_executable(axc-gui-test
        test/gui/session_test.cpp
        src/gui/debug/RunnerProcess.cpp
        src/gui/debug/Session.cpp
        src/gui/debug/VariablesModel.cpp
        src/gui/debug/SessionManager.cpp
        src/gui/debug/PerfSampler.cpp
        src/gui/EngineObject.cpp
        src/gui/BreakpointStore.cpp
        src/gui/Settings.cpp
        src/proto/message.cpp
    )

    set_target_properties(axc-gui-test PROPERTIES AUTOMOC ON)

    target_include_directories(axc-gui-test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/gui
            ${ALXLIB_INC}/alxcomm
    )

    target_link_libraries(axc-gui-test PRIVATE
        axc-core
        ${ALXLIB_RUNTIME}/libalxcomm.so
        Qt6::Core
        Qt6::Gui
    )

    set_target_properties(axc-gui-test PROPERTIES
        BUILD_RPATH "$ORIGIN;$ORIGIN/alxlib"
        INSTALL_RPATH "$ORIGIN;$ORIGIN/alxlib"
    )

    add_executable(axc-hint-test
        test/gui/hint_test.cpp
        src/gui/HintProvider.cpp
    )

    target_include_directories(axc-hint-test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/gui
            ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    )

    target_link_libraries(axc-hint-test PRIVATE
        axc-core
        Qt6::Core
    )

    set_target_properties(axc-hint-test PROPERTIES
        BUILD_RPATH "$ORIGIN;$ORIGIN/alxlib"
        INSTALL_RPATH "$ORIGIN;$ORIGIN/alxlib"
    )
endif()

# ---------------------------------------------------------------------------
# Split debug info
# ---------------------------------------------------------------------------
if(ASCRIPT_SPLIT_DEBUG)
    foreach(target axc-core)
        if(TARGET ${target})
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND objcopy --only-keep-debug $<TARGET_FILE:${target}> $<TARGET_FILE:${target}>.debug
                COMMAND objcopy --strip-debug $<TARGET_FILE:${target}>
                COMMAND objcopy --add-gnu-debuglink=$<TARGET_FILE:${target}>.debug $<TARGET_FILE:${target}>
                COMMENT "Splitting debug info for ${target}"
            )
        endif()
    endforeach()

    foreach(target axc-cli axc-gui)
        if(TARGET ${target})
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND objcopy --only-keep-debug $<TARGET_FILE:${target}> $<TARGET_FILE:${target}>.debug
                COMMAND objcopy --strip-debug $<TARGET_FILE:${target}>
                COMMAND objcopy --add-gnu-debuglink=$<TARGET_FILE:${target}>.debug $<TARGET_FILE:${target}>
                COMMENT "Splitting debug info for ${target}"
            )
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Link libraries
# ---------------------------------------------------------------------------
if(BUILD_LINKS)
    set(LINK_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/bin/links)
    file(MAKE_DIRECTORY ${LINK_OUTPUT_DIR})

    # each link has its own CMakeLists.txt
    file(GLOB LINK_DIRS ${CMAKE_SOURCE_DIR}/links/*)
    foreach(LINK_DIR ${LINK_DIRS})
        if(IS_DIRECTORY ${LINK_DIR} AND EXISTS ${LINK_DIR}/CMakeLists.txt)
            get_filename_component(LINK_NAME ${LINK_DIR} NAME)
            message(STATUS "Building link: ${LINK_NAME}")
            add_subdirectory(${LINK_DIR} ${CMAKE_BINARY_DIR}/links/${LINK_NAME})
        endif()
    endforeach()
endif()
