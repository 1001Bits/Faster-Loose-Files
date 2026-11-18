cmake_minimum_required(VERSION 3.23)

set(NAME "FasterLooseFiles")
set(VERSION 1.0.1)

project(${NAME} VERSION ${VERSION} LANGUAGES CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR "In-source builds not allowed.")
endif()

# The C runtime is built in: the plugin needs no Visual C++ redistributable of its own.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

set(SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_compile_definitions(
    FLF_VERSION="${PROJECT_VERSION}"
    FLF_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    FLF_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    FLF_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

set(COMMON_OPTIONS /W4 /permissive- /Zc:preprocessor /Zc:__cplusplus /Zi /utf-8 /MP)

# The reading code alone: the plugin and the tests use the same.
add_library(LooseReadsCore STATIC "${SOURCE_DIR}/LooseReads.cpp" "${SOURCE_DIR}/LooseReads.h")
target_compile_features(LooseReadsCore PUBLIC cxx_std_23)
target_include_directories(LooseReadsCore PUBLIC "${SOURCE_DIR}")
target_compile_options(LooseReadsCore PRIVATE ${COMMON_OPTIONS})

set(VERSION_RC "${CMAKE_CURRENT_BINARY_DIR}/version.rc")
file(WRITE "${VERSION_RC}" "#include <winres.h>
1 VERSIONINFO
FILEVERSION ${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0
PRODUCTVERSION ${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0
FILEOS VOS__WINDOWS32
FILETYPE VFT_DLL
BEGIN
    BLOCK \"StringFileInfo\"
    BEGIN
        BLOCK \"040904B0\"
        BEGIN
            VALUE \"FileDescription\", \"Faster Loose Files F4SE plugin\"
            VALUE \"FileVersion\", \"${PROJECT_VERSION}\"
            VALUE \"InternalName\", \"${PROJECT_NAME}\"
            VALUE \"OriginalFilename\", \"${PROJECT_NAME}.dll\"
            VALUE \"ProductName\", \"Faster Loose Files\"
            VALUE \"ProductVersion\", \"${PROJECT_VERSION}\"
        END
    END
    BLOCK \"VarFileInfo\"
    BEGIN
        VALUE \"Translation\", 0x409, 1200
    END
END
")

add_library(${PROJECT_NAME} SHARED
    "${SOURCE_DIR}/main.cpp"
    "${SOURCE_DIR}/Log.cpp"
    "${SOURCE_DIR}/Log.h"
    "${SOURCE_DIR}/F4se.h"
    "${SOURCE_DIR}/Game.h"
    "${SOURCE_DIR}/Settings.h"
    "${VERSION_RC}")
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_compile_options(${PROJECT_NAME} PRIVATE ${COMMON_OPTIONS})
target_link_libraries(${PROJECT_NAME} PRIVATE LooseReadsCore version shell32 ole32)
target_link_options(${PROJECT_NAME} PRIVATE /DEBUG /OPT:REF /OPT:ICF)

# Offline tests of the reading code, against real files.
enable_testing()
add_executable(LooseReadsTest "${CMAKE_CURRENT_SOURCE_DIR}/tests/LooseReadsTest.cpp")
target_compile_options(LooseReadsTest PRIVATE ${COMMON_OPTIONS})
target_link_libraries(LooseReadsTest PRIVATE LooseReadsCore)
# Another plugin's reads through its own imports, as Creation Engine Tweaks makes them; next to the test.
add_library(OtherReader SHARED "${CMAKE_CURRENT_SOURCE_DIR}/tests/OtherReader.cpp")
target_compile_options(OtherReader PRIVATE ${COMMON_OPTIONS})
add_dependencies(LooseReadsTest OtherReader)
# Optionally also replays recorded game reads: -DLOOSE_READS_TRACES="trace1.txt;trace2.txt".
set(LOOSE_READS_TRACES "" CACHE STRING "Recorded loose file reads of the game, replayed by the test")
add_test(NAME loose-reads COMMAND LooseReadsTest ${LOOSE_READS_TRACES})

# Data/F4SE/Plugins as it installs.
set(PLUGINS_DIR "${CMAKE_CURRENT_BINARY_DIR}/package/F4SE/Plugins")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PLUGINS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${PLUGINS_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${PROJECT_NAME}> "${PLUGINS_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/FasterLooseFiles.ini" "${PLUGINS_DIR}/")

# The release archive: only this plugin's three files, named, so nothing else in the package folder slips in.
set(TARGET_ZIP "${PROJECT_NAME}-Fallout4-${PROJECT_VERSION}.7z")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_ZIP}"
    COMMAND ${CMAKE_COMMAND} -E tar cf "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_ZIP}" --format=7zip --
        F4SE/Plugins/FasterLooseFiles.dll F4SE/Plugins/FasterLooseFiles.pdb F4SE/Plugins/FasterLooseFiles.ini
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/package")
