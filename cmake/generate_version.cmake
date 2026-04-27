# generate_version.cmake — git commit count からバージョン header を生成
#
# 呼び出し元から以下の変数を渡すこと:
#   WORKING_DIR   — git リポジトリのルート
#   VERSION_FILE  — 出力する version.h のパス
#   VERSION_MACRO — #define するマクロ名 (例: BBB_OSC_VERSION)

execute_process(
    COMMAND git rev-list --count HEAD
    WORKING_DIRECTORY ${WORKING_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_COUNT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if(NOT GIT_COMMIT_COUNT)
    set(GIT_COMMIT_COUNT 1)
endif()

set(VERSION_STRING "0.0.${GIT_COMMIT_COUNT}")

set(_content "#pragma once\n#define ${VERSION_MACRO} \"${VERSION_STRING}\"\n")

if(EXISTS ${VERSION_FILE})
    file(READ ${VERSION_FILE} _existing)
    if(_existing STREQUAL _content)
        return()
    endif()
endif()

file(WRITE ${VERSION_FILE} ${_content})
