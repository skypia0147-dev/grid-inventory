cmake_minimum_required(VERSION 3.21)

# --- Force-disable CommonLibSSE-NG test/tooling options (prevent Catch2/regex conflicts) ---
option(CommonLibSSE_POSH_GENERATION "Generate .posh.d.ts type definition file" OFF)
option(CommonLibSSE_TESTING "Build tests" OFF)
set(CommonLibSSE_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(GridInventory VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Packages (order matters) ---
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

# CommonLibSSE-NG expects RAPIDCSV_INCLUDE_DIRS (header-only rapidcsv from vcpkg).
find_path(RAPIDCSV_INCLUDE_DIRS "rapidcsv.h")

# --- CommonLibSSE-NG from source -----------------------------------------
# ★★★MOVED OFF CharmedBaryon FOR SKYRIM 1.7.99.
#
# That tree decides the runtime from the MINOR version with a switch:
#
#     case 4: VR    case 6: AE    default: SE
#
# so 1.7.99 is classified SE. Two things follow and both are fatal: the
# address library is looked for under the SE name (version-1-7-99-0.bin
# instead of versionlib-...), which is the "failed to open address library
# file" every 1.7.99 player is reporting, and every RELOCATION_ID(se, ae)
# picks the SE id -- the wrong address for every call we make.
#
# Upstream main has not shipped a fix (checked: origin/main is the same commit
# we already had, latest tag v3.7.0). alandtse's NG line has:
#
#     if (v[1] == 4) VR; else if (v[1] >= 6) AE; else SE;
#
# and it reads the address library's FORMAT from the file rather than assuming
# it from the runtime -- which is what Address Library v12 needs.
#
# ★Pinned to a COMMIT, not to `ng`. A moving branch would put code we have not
# looked at into the next build of a plugin whose failure mode is a crash on
# someone else's machine. Bump it deliberately.
#
# Same project name and target (CommonLibSSE::CommonLibSSE), same licence
# (GPL-3.0). It brings one new dependency: directxtk.
include(FetchContent)
FetchContent_Declare(
    CommonLibSSE
    GIT_REPOSITORY https://github.com/alandtse/CommonLibVR.git
    GIT_TAG 0e167dd4c3981d586a53953e2c1a608e2c4ccedf   # ng @ 2026-08-21
)
FetchContent_MakeAvailable(CommonLibSSE)

# --- Plugin target ---
set(SOURCES
    src/PCH.h
    src/main.cpp
    src/api/GridInventoryAPI.h
    src/api/HostApi.h
    src/api/HostApi.cpp
    src/game/Inv3D.h
    src/game/GoldCoins.h
    src/game/GoldCoins.cpp
    src/game/BagFilter.h
    src/game/BagFilter.cpp
    src/game/Costume.h
    src/game/Costume.cpp
    src/game/DualRing.h
    src/game/DualRing.cpp
    src/game/DeltaWatch.h
    src/game/DeltaWatch.cpp
    src/game/Census.h
    src/game/Census.cpp
    src/game/Ledger.h
    src/game/Ledger.cpp
    src/game/WornLedger.h
    src/game/WornLedger.cpp
    src/game/Lotd.h
    src/game/Lotd.cpp
    src/ui/ItemPreview.h
    src/ui/ItemPreview.cpp
    src/ui/GridMenu.h
    src/ui/GridMenu.cpp
    src/ui/Badges.h
    src/ui/Badges.cpp
    src/ui/UIRoot.h
    src/ui/UIRoot.cpp
    src/ui/IconCache.h
    src/ui/IconCache.cpp

    src/ui/Fallback.cpp
    src/ui/WinManager.h
    src/ui/WinManager.cpp
    src/ui/Grid.h
    src/ui/Grid.cpp
    src/ui/LootBarter.h
    src/ui/LootBarter.cpp
    src/ui/Equip.h
    src/ui/Equip.cpp
    src/ui/Loadout.h
    src/ui/Loadout.cpp
    src/ui/Editor.h
    src/ui/Editor.cpp
    src/ui/Theme.h
    src/ui/Theme.cpp
    src/ui/Lang.h
    src/ui/Lang.cpp
    src/ui/Wheeler.h
    src/ui/Wheeler.cpp
)

add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_compile_options(${PROJECT_NAME} PRIVATE /utf-8 /Zi)   # UTF-8 string literals (i18n) + PDB
# PDB for crash-log symbolication; keep release layout (/OPT defaults flip with /DEBUG)
target_link_options(${PROJECT_NAME} PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_include_directories(${PROJECT_NAME} PRIVATE src)

target_link_libraries(${PROJECT_NAME} PRIVATE
    CommonLibSSE::CommonLibSSE
    fmt::fmt
    spdlog::spdlog
    imgui::imgui
    d3d11
    d3dcompiler     # the silhouette pixel shader (Windows SDK, same as d3d11)
    windowscodecs   # WIC: PNG decoding for the drawn icon set (Windows SDK)
)

# --- Icon-capture diagnostics (OFF; never ship enabled) ---
# Builds a plugin that reads the capture rect back to the CPU and logs what it
# found. That readback stalls the render thread, so it exists only to hand a
# one-off build to a user whose environment cannot be reproduced here -- an
# upscaler changes the backbuffer's format, size and contents, and guessing at
# which one from a screenshot is how days get spent.
option(GI_CAPTURE_DIAG "Log icon capture surface + pixel stats (SLOW)" OFF)
if(GI_CAPTURE_DIAG)
    target_compile_definitions(${PROJECT_NAME} PRIVATE GI_CAPTURE_DIAG)
    message(STATUS "GridInventory: icon capture diagnostics ENABLED (slow build)")
endif()

# --- Deploy DLL to a local MO2 mod folder (developer convenience) ---
# Point MOD_ROOT at your own mod folder (-DMOD_ROOT=...). The copy step is
# skipped entirely when the folder does not exist, so a plain clone builds
# without any local setup.
set(MOD_ROOT "H:/Game/Fablerim/mods/Grid Inventory" CACHE PATH
    "MO2 mod folder the built DLL is copied into (skipped if absent)")
if(EXISTS "${MOD_ROOT}")
    set(DLL_DEPLOY "${MOD_ROOT}/SKSE/Plugins")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DLL_DEPLOY}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_DEPLOY}/"
        VERBATIM
    )
endif()
