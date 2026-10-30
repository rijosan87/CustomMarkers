# It's recommended to set a minimum CMake version.
# If you use CMake features from higher versions, update this to match.
cmake_minimum_required(VERSION 3.21)
# Set your project name. This will be the name of your SKSE .dll file.
project(CustomMarkers VERSION 1.3.2 LANGUAGES CXX)
#
# YOU DO NOT NEED TO EDIT ANYTHING BELOW HERE
#
# Please see the "Project setup" section of README.md
#
# If you're not using a mod manager, you probably want the SKSE plugin to go
# inside of your Skyrim "Data" folder.
#
# To do this automatically, set the `SKYRIM_FOLDER` environment variable
# to the path of your Skyrim Special Edition folder
if(DEFINED ENV{SKYRIM_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_FOLDER}/Data")
    set(OUTPUT_FOLDER "$ENV{SKYRIM_FOLDER}/Data")
endif()
# If you're using Mod Organizer 2 or Vortex, you might want this to go inside
# of your "mods" folder, inside of a subfolder named "<your mod>".
#
# To do this automatically, set the `SKYRIM_MODS_FOLDER` environment variable
# to the path of your "mods" folder
if(DEFINED ENV{SKYRIM_MODS_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_MODS_FOLDER}")
    set(OUTPUT_FOLDER "$ENV{SKYRIM_MODS_FOLDER}/${PROJECT_NAME}")
endif()
# Otherwise, you can set OUTPUT_FOLDER to any place you'd like :)
# set(OUTPUT_FOLDER "C:/path/to/any/folder")
# Setup your SKSE plugin as an SKSE plugin!
# CommonLibSSE-NG 7.x from a local checkout: the colorglass vcpkg registry is stuck on
# 3.7.0, which predates 1.7.99/1.7.104 support (runtime classification + format-5 address library).
# 7.x is GPL-3.0-or-later where 3.7.0 was MIT, hence COPYING at the project root.
# Do NOT resolve this by environment variable alone: Visual Studio captures the environment
# at launch, so setting CommonLibSSEPath while VS is open leaves configure failing until a
# full restart, with an error that reads like the checkout is missing.
if(DEFINED ENV{CommonLibSSEPath})
    file(TO_CMAKE_PATH "$ENV{CommonLibSSEPath}" CLIB_SOURCE_DIR)
elseif(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/CommonLibSSE-NG")
    set(CLIB_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/CommonLibSSE-NG")
elseif(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibSSE-NG")
    set(CLIB_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibSSE-NG")
endif()

if(NOT CLIB_SOURCE_DIR OR NOT EXISTS "${CLIB_SOURCE_DIR}/cmake/CommonLibSSE.cmake")
    message(FATAL_ERROR
        "CommonLibSSE-NG 7.x checkout not found. Clone CommonLibSSE-NG into the project folder "
        "(https://github.com/alandtse/CommonLibSSE-NG), place it in extern/CommonLibSSE-NG, "
        "or set the CommonLibSSEPath environment variable.")
endif()

message(STATUS "CommonLibSSE-NG: ${CLIB_SOURCE_DIR}")

# Every set(... CACHE ... FORCE) must come BEFORE add_subdirectory: option() inside NG
# cannot override an existing cache entry.
# NG defaults BUILD_TESTS ON, which does find_package(Catch2 REQUIRED) and fails configure.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)

# SE+AE runtime dispatch = one DLL for SE 1.5.97, AE 1.6.1170 and 1.7.99.
# VR off: we don't target it, and it links openvr_api.lib from an uninitialized submodule.
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)

add_subdirectory("${CLIB_SOURCE_DIR}" CommonLibSSE-NG)

# NG only includes this on its prebuilt path (which return()s early), so a source build
# never defines add_commonlibsse_plugin().
include("${CLIB_SOURCE_DIR}/cmake/CommonLibSSE.cmake")
find_package(PolyHook_2 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(directxtex CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(lunasvg CONFIG REQUIRED)
# MINIMUM_SKSE_VERSION must be a full x.y.z triplet: the generator splits on "."
# and a bare "0" (the default) emits REL::Version{ 0, , , 0 } - a syntax error.
add_commonlibsse_plugin(${PROJECT_NAME} MINIMUM_SKSE_VERSION 0.0.0 SOURCES src/AutomationTelemetry.cpp src/plugin.cpp src/MarkerManager.cpp src/MapMarkerRenderer.cpp src/LocalMapRenderer.cpp src/LocalMapClickHook.cpp src/MapHoverHook.cpp src/Settings.cpp src/LocalizationManager.cpp src/PlaceMarkerHook.cpp src/MarkerClickHook.cpp src/MarkerHoverHook.cpp src/MarkerFilterHook.cpp src/MapLookRemapHook.cpp src/CompassHook.cpp src/HUDMarkerHook.cpp src/ImGui/Renderer.cpp src/InputHook.cpp src/TextureManager.cpp src/BackgroundManager.cpp src/HoverImageRenderer.cpp src/PathTracker.cpp src/MinimapRenderer.cpp src/MinimapSnapshotTransition.cpp src/MinimapEntityTransition.cpp src/WorldBeaconRenderer.cpp src/LootBeamRenderer.cpp src/LootBeamDesignModern.cpp src/LootBeamDesignWisp.cpp src/LootBeamBlacklist.cpp src/TCCItemStatus.cpp src/LookPinRenderer.cpp src/RenderTargetUpscaler.cpp src/EntityMarkerSource.cpp src/ActorMarkerSource.cpp src/ImGui/SettingsMenu.cpp src/ImGui/SettingsMenuFramework.cpp src/FontManager.cpp src/MapStats.cpp src/QuestTracker.cpp src/CompanionTracker.cpp src/InputHandler.cpp)# <--- specifies plugin.cpp
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23) # <--- use C++23 standard
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h) # <--- PCH.h is required!

# Always emit a matching PDB, even on Release, so a user's crash-log offsets can be
# resolved later. /Zi generates full debug info; /DEBUG writes the PDB; /OPT:REF /OPT:ICF
# keep the release binary optimized (debug info and optimization coexist fine).
# The PDB is archived (below), NOT shipped with the mod.
target_compile_options(${PROJECT_NAME} PRIVATE /Zi)
target_link_options(${PROJECT_NAME} PRIVATE /DEBUG /OPT:REF /OPT:ICF)
# Add include directories
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Link PolyHook2 for runtime function-entry detours and ImGui for rendering
target_link_libraries(${PROJECT_NAME} PRIVATE PolyHook_2::PolyHook_2 imgui::imgui Microsoft::DirectXTex nlohmann_json::nlohmann_json lunasvg::lunasvg)
# Silence the stdext warnings
target_compile_definitions(${PROJECT_NAME} PRIVATE _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING)
# When your SKSE .dll is compiled, this will automatically copy the .dll into your mods folder.
# Only works if you configure DEPLOY_ROOT above (or set the SKYRIM_MODS_FOLDER environment variable)
if(DEFINED OUTPUT_FOLDER)
    # If you specify an <OUTPUT_FOLDER> (including via environment variables)
    # then we'll copy your mod files into Skyrim or a mod manager for you!
    # Copy the SKSE plugin .dll files into the SKSE/Plugins/ folder
    set(DLL_FOLDER "${OUTPUT_FOLDER}/SKSE/Plugins")
    message(STATUS "SKSE plugin output folder: ${DLL_FOLDER}")
    # Copy runtime DLLs (add this section)
	
    add_custom_command(
        TARGET "${PROJECT_NAME}"
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        VERBATIM
    )
    # If you perform a "Debug" build, also copy .pdb file (for debug symbols)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_custom_command(
            TARGET "${PROJECT_NAME}"
            POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>"
            VERBATIM
        )
    endif()
endif()

# Archive the PDB (symbols) for every build into a version-stamped folder
set(PDB_ARCHIVE "${CMAKE_SOURCE_DIR}/pdb-archive/${PROJECT_VERSION}")
add_custom_command(
    TARGET "${PROJECT_NAME}"
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PDB_ARCHIVE}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:${PROJECT_NAME}>" "${PDB_ARCHIVE}/$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${PDB_ARCHIVE}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
    VERBATIM
)
