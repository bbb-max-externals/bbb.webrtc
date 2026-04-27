# bbb_add_external — Max/MSP external を min-api でビルドする共通 macro
#
# usage (in source/projects/<name>/CMakeLists.txt):
#   bbb_add_external(
#       [MACOS_ONLY]                    # macOS のみビルド (Windows ではスキップ)
#       [WIN32_ONLY]                    # Windows のみビルド (macOS ではスキップ)
#       [DEPS lib1 lib2 ...]            # target_link_libraries に渡す依存
#       [INCLUDES dir1 dir2 ...]        # target_include_directories に渡す追加パス
#       [SOURCES file1.cpp ...]         # 追加ソース (省略時: *.cpp を自動収集)
#       [RPATH path]                    # BUILD_RPATH / INSTALL_RPATH を設定
#       [NO_HELP_COPY]                  # help ファイルの自動コピーを無効化
#   )
#
# 前提:
#   - PROJECT_NAME が external 名と一致していること
#     (CMakeLists.txt と同名ディレクトリに配置すれば自動設定される)
#   - ルート CMakeLists.txt で以下のいずれかが設定済みであること:
#     a) C74_MIN_API_DIR 変数
#     b) deps/min-api/ が存在する (自動推測する)
#   - C74_LIBRARY_OUTPUT_DIRECTORY が設定済みであること (省略時は <root>/externals)

macro(bbb_add_external)
    cmake_parse_arguments(BBB_ARG
        "NO_HELP_COPY;MACOS_ONLY;WIN32_ONLY"
        "RPATH"
        "DEPS;INCLUDES;SOURCES"
        ${ARGN}
    )

    # --- platform guard ---
    set(_bbb_should_build TRUE)
    if(BBB_ARG_MACOS_ONLY AND NOT APPLE)
        set(_bbb_should_build FALSE)
    endif()
    if(BBB_ARG_WIN32_ONLY AND NOT WIN32)
        set(_bbb_should_build FALSE)
    endif()

    if(_bbb_should_build)

    # --- min-api path resolution ---
    if(NOT DEFINED C74_MIN_API_DIR)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../deps/min-api")
            set(C74_MIN_API_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../deps/min-api")
        elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/min-api")
            set(C74_MIN_API_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/min-api")
        else()
            message(FATAL_ERROR "bbb_add_external: C74_MIN_API_DIR not set and min-api not found")
        endif()
    endif()

    # --- output directory ---
    if(NOT DEFINED C74_LIBRARY_OUTPUT_DIRECTORY)
        set(C74_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../../externals")
    endif()

    # --- universal binary (macOS) ---
    if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "macOS architecture" FORCE)
    endif()

    # --- collect sources ---
    if(BBB_ARG_SOURCES)
        set(_bbb_sources ${BBB_ARG_SOURCES})
    else()
        file(GLOB _bbb_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
    endif()

    # --- min-api pre-target ---
    include(${C74_MIN_API_DIR}/script/min-pretarget.cmake)

    # --- propagate directory-level variables to parent scope ---
    # min-pretarget -> max-pretarget sets CMAKE_*_LINKER_FLAGS and
    # CMAKE_*_OUTPUT_DIRECTORY.  These are directory-scope variables
    # that the CMake generator reads when producing link commands and output
    # paths.  Because we are inside a function(), changes to these variables
    # are confined to the function scope and silently dropped on return.
    # Without PARENT_SCOPE the generated link command omits the -Wl,-U flags
    # from max-linker-flags.txt, causing "Undefined symbols" at link time.
    # NOTE: Standard and custom build configurations are propagated.
    # CMAKE_CONFIGURATION_TYPES (multi-config) and CMAKE_BUILD_TYPE
    # (single-config) are included alongside the four standard configs.
    foreach(_var CMAKE_MODULE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS
                 CMAKE_EXE_LINKER_FLAGS CMAKE_STATIC_LINKER_FLAGS
                 CMAKE_C_FLAGS CMAKE_CXX_FLAGS CMAKE_MSVC_RUNTIME_LIBRARY
                 CMAKE_LIBRARY_OUTPUT_DIRECTORY CMAKE_RUNTIME_OUTPUT_DIRECTORY
                 CMAKE_ARCHIVE_OUTPUT_DIRECTORY CMAKE_PDB_OUTPUT_DIRECTORY
                 CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY
                 CMAKE_INTERPROCEDURAL_OPTIMIZATION CMAKE_POSITION_INDEPENDENT_CODE
                 CMAKE_OSX_DEPLOYMENT_TARGET)
        if(DEFINED ${_var})
            set(${_var} "${${_var}}" PARENT_SCOPE)
        endif()
        foreach(_config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL
                       ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
            string(TOUPPER "${_config}" _config_upper)
            if(DEFINED ${_var}_${_config_upper})
                set(${_var}_${_config_upper} "${${_var}_${_config_upper}}" PARENT_SCOPE)
            endif()
        endforeach()
    endforeach()

    # --- build library ---
    add_library(${PROJECT_NAME} MODULE ${_bbb_sources})

    # --- include directories ---
    target_include_directories(${PROJECT_NAME} PRIVATE ${C74_INCLUDES})
    if(BBB_ARG_INCLUDES)
        target_include_directories(${PROJECT_NAME} PRIVATE ${BBB_ARG_INCLUDES})
    endif()

    # --- link dependencies ---
    if(BBB_ARG_DEPS)
        target_link_libraries(${PROJECT_NAME} PRIVATE ${BBB_ARG_DEPS})
    endif()

    # --- rpath (for externals that load shared libraries at runtime) ---
    if(BBB_ARG_RPATH)
        set_target_properties(${PROJECT_NAME} PROPERTIES
            BUILD_RPATH "${BBB_ARG_RPATH}"
            INSTALL_RPATH "${BBB_ARG_RPATH}"
        )
    endif()

    # --- help file copy ---
    if(NOT BBB_ARG_NO_HELP_COPY)
        set(_bbb_help_src "${CMAKE_CURRENT_SOURCE_DIR}/${PROJECT_NAME}.maxhelp")
        set(_bbb_help_dst "${CMAKE_CURRENT_SOURCE_DIR}/../../../help/${PROJECT_NAME}.maxhelp")
        if(EXISTS "${_bbb_help_src}")
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_bbb_help_src}" "${_bbb_help_dst}"
            )
        endif()
    endif()

    # --- min-api post-target ---
    include(${C74_MIN_API_DIR}/script/min-posttarget.cmake)

    endif() # _bbb_should_build

    # --- cleanup: unset internal variables to avoid scope pollution (macro shares caller scope) ---
    unset(_bbb_sources)
    unset(_bbb_help_src)
    unset(_bbb_help_dst)
    unset(_bbb_should_build)
    unset(BBB_ARG_NO_HELP_COPY)
    unset(BBB_ARG_MACOS_ONLY)
    unset(BBB_ARG_WIN32_ONLY)
    unset(BBB_ARG_RPATH)
    unset(BBB_ARG_DEPS)
    unset(BBB_ARG_INCLUDES)
    unset(BBB_ARG_SOURCES)
    unset(BBB_ARG_UNPARSED_ARGUMENTS)
    unset(BBB_ARG_KEYWORDS_MISSING_VALUES)
endmacro()
