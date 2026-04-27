cmake_minimum_required(VERSION 3.19)
project(bbb.webrtc)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- load bbb_add_external() ---
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/bbb_external.cmake)

# --- output directory ---
set(C74_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/externals)

# --- universal binary (macOS) ---
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
	set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "macOS architecture" FORCE)
endif()

# --- libdatachannel (WebRTC with media support) ---
include(FetchContent)
FetchContent_Declare(
	libdatachannel
	GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
	GIT_TAG        v0.22.6
)
set(NO_MEDIA OFF CACHE BOOL "Enable media support" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "Disable built-in WebSocket" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
if(MSVC)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
endif()
FetchContent_MakeAvailable(libdatachannel)

# --- opus (audio codec) ---
FetchContent_Declare(
	opus
	GIT_REPOSITORY https://github.com/xiph/opus.git
	GIT_TAG        v1.5.2
)
set(OPUS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(opus)

# --- shared webrtc library ---
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/source/webrtc)

# --- version generation ---
string(MAKE_C_IDENTIFIER "${PROJECT_NAME}" BBB_PACKAGE_NAME_UPPER)
string(TOUPPER "${BBB_PACKAGE_NAME_UPPER}" BBB_PACKAGE_NAME_UPPER)
set(BBB_VERSION_MACRO "${BBB_PACKAGE_NAME_UPPER}_VERSION")
add_custom_target(generate_version ALL
	COMMAND "${CMAKE_COMMAND}"
		-DWORKING_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
		-DVERSION_FILE="${CMAKE_CURRENT_SOURCE_DIR}/source/bbb/version.h"
		-DVERSION_MACRO="${BBB_VERSION_MACRO}"
		-P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_version.cmake"
)

# --- auto-discover external projects ---
MACRO(SUBDIRLIST result curdir)
	FILE(GLOB children RELATIVE ${curdir} ${curdir}/*)
	SET(dirlist "")
	FOREACH(child ${children})
		IF(IS_DIRECTORY ${curdir}/${child})
			LIST(APPEND dirlist ${child})
		ENDIF()
	ENDFOREACH()
	SET(${result} ${dirlist})
ENDMACRO()

SUBDIRLIST(PROJECT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/source/projects)
foreach(project_dir ${PROJECT_DIRS})
	if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/source/projects/${project_dir}/CMakeLists.txt")
		message("Generating: ${project_dir}")
		add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/source/projects/${project_dir})
	endif()
endforeach()

set(CMAKE_SUPPRESS_REGENERATION true)
