cmake_minimum_required(VERSION 3.16)
project(live-latent-lab-sound VERSION 0.3.0 LANGUAGES C CXX)

# All effects quarantined by default; opt into a named repair build.
set(SOUND_EFFECTS "" CACHE STRING "Semicolon-separated experimental effect IDs")
option(SOUND_EXPERIMENTAL "Permit explicitly selected unverified effects" OFF)
option(SOUND_TOOLS_MENU "Include optional Qt Tools menu" OFF)
option(SOUND_DIFFUSION_DOCK "Include native Qt Audio Diffusion dock" OFF)
option(SOUND_SETTINGS_PAGE "Include native Qt Plugins settings page" ${SOUND_DIFFUSION_DOCK})
set(SOUND_SUPPORTED_EFFECTS
    stem_live yamnet tts stt diffusion ddsp rvc deepfilter demucs_filter demucs_source whisper
    chatterbox kokoro matchering musicgen mediapipe meanvc2_bridge)
set(SOUND_SELECTED_EFFECTS "")
foreach(effect IN LISTS SOUND_EFFECTS)
    if(NOT effect IN_LIST SOUND_SUPPORTED_EFFECTS)
        message(FATAL_ERROR "Unknown Sound effect '${effect}'. Allowed: ${SOUND_SUPPORTED_EFFECTS}")
    endif()
    if(effect IN_LIST SOUND_SELECTED_EFFECTS)
        message(FATAL_ERROR "Duplicate Sound effect '${effect}'")
    endif()
    list(APPEND SOUND_SELECTED_EFFECTS "${effect}")
endforeach()
if((stem_live IN_LIST SOUND_SELECTED_EFFECTS OR ddsp IN_LIST SOUND_SELECTED_EFFECTS OR rvc IN_LIST SOUND_SELECTED_EFFECTS) AND NOT (UNIX AND NOT APPLE))
    message(FATAL_ERROR "Buffered stem, DDSP and native RVC filters currently support Linux only")
endif()
if(SOUND_SELECTED_EFFECTS AND NOT SOUND_EXPERIMENTAL)
    message(FATAL_ERROR "Quarantine: enable -DSOUND_EXPERIMENTAL=ON for individual effects")
endif()
message(STATUS "Sound enabled effects: [${SOUND_SELECTED_EFFECTS}]")
if(SOUND_DIFFUSION_DOCK AND NOT diffusion IN_LIST SOUND_SELECTED_EFFECTS)
    message(FATAL_ERROR "Audio Diffusion dock requires the diffusion effect")
endif()

# ===== PLATFORM & LANGUAGE =====
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Enable optimization flags
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")
endif()

# Detect platform
if(WIN32)
    set(OS_PLATFORM "windows")
elseif(APPLE)
    set(OS_PLATFORM "macos")
elseif(UNIX)
    set(OS_PLATFORM "linux")
endif()

# =============================================================================
# Qt6 Integration (OBS Compatible)
# =============================================================================
if(SOUND_TOOLS_MENU OR SOUND_DIFFUSION_DOCK OR SOUND_SETTINGS_PAGE)
    find_package(Qt6 COMPONENTS Core Gui Widgets REQUIRED)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_INCLUDE_CURRENT_DIR ON)
endif()

# =============================================================================
# OBS Library Detection & Fetch
# =============================================================================
include(FetchContent)

option(FETCH_OBS "Fetch OBS Studio source" OFF)

if(OBS_DIR)
    message(STATUS "Using Manual OBS_DIR: ${OBS_DIR}")
    set(LibOBS_INCLUDE_DIR 
        "${OBS_DIR}/libobs"
        "${OBS_DIR}/build/config")
    set(LibOBS_LIBRARY "${OBS_DIR}/build/libobs/libobs.so")
elseif(FETCH_OBS)
    message(STATUS "Fetching OBS Studio (latest)...")
    FetchContent_Declare(
        obs-studio
        GIT_REPOSITORY https://github.com/obsproject/obs-studio.git
        GIT_TAG        31.1.1
        GIT_SHALLOW    TRUE
        GIT_SUBMODULES ""
    )
    # We only want the headers/source, we don't want to build the full app if we can avoid it,
    # but we need to ensure headers are available.
    FetchContent_GetProperties(obs-studio)
    if(NOT obs-studio_POPULATED)
        FetchContent_Populate(obs-studio)
    endif()

    # Generate obsconfig.h
    set(OBS_VERSION_MAJOR 31)
    set(OBS_VERSION_MINOR 1)
    set(OBS_VERSION_PATCH 1)
    set(OBS_RELEASE_CANDIDATE 0)
    set(OBS_BETA 0)
    set(OBS_DATA_PATH "/usr/share/obs/data")
    set(OBS_PLUGIN_PATH "/usr/lib/obs-plugins")

    configure_file(
        "${obs-studio_SOURCE_DIR}/libobs/obsconfig.h.in"
        "${obs-studio_BINARY_DIR}/config/obsconfig.h"
    )

    set(LibOBS_INCLUDE_DIR
        "${obs-studio_SOURCE_DIR}/libobs"
        "${obs-studio_BINARY_DIR}/config"
    )

    # On Linux, we can build a module with undefined symbols if we don't have the lib.
    set(LibOBS_LIBRARY "")

else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBOBS libobs)
    if(LIBOBS_FOUND)
        set(LibOBS_LIBRARY ${LIBOBS_LIBRARIES})
        find_path(OBS_HEADER_PATH obs-module.h
            PATHS 
                ${LIBOBS_INCLUDE_DIRS}
                /usr/include/obs
                /usr/local/include/obs
            PATH_SUFFIXES libobs obs
        )
        if(OBS_HEADER_PATH)
            set(LibOBS_INCLUDE_DIR ${OBS_HEADER_PATH})
            message(STATUS "Found obs-module.h at: ${OBS_HEADER_PATH}")
        else()
            set(LibOBS_INCLUDE_DIR ${LIBOBS_INCLUDE_DIRS})
        endif()
    else()
        message(STATUS "LibOBS not found via pkg-config, searching standard paths...")
        find_library(LIBOBS_LIBRARY obs)
        find_path(LibOBS_INCLUDE_DIR obs-module.h 
            PATHS /usr/include /usr/local/include
            PATH_SUFFIXES libobs obs)
        if (NOT LIBOBS_LIBRARY OR NOT LibOBS_INCLUDE_DIR)
             message(FATAL_ERROR "LibOBS not found. Please set OBS_DIR or enable FETCH_OBS.")
        endif()
    endif()
endif()

message(STATUS "LibOBS Include: ${LibOBS_INCLUDE_DIR}")

if(SOUND_SELECTED_EFFECTS)
set(SOUND_NEEDS_ONNX FALSE)
foreach(effect IN LISTS SOUND_SELECTED_EFFECTS)
    if(NOT effect STREQUAL "mic" AND NOT effect STREQUAL "matchering" AND
       NOT effect STREQUAL "meanvc2_bridge" AND
       NOT effect STREQUAL "stem_live" AND
       NOT effect STREQUAL "yamnet" AND NOT effect STREQUAL "tts" AND NOT effect STREQUAL "stt" AND NOT effect STREQUAL "diffusion" AND NOT effect STREQUAL "ddsp" AND NOT effect STREQUAL "rvc")
        set(SOUND_NEEDS_ONNX TRUE)
    endif()
endforeach()
if(SOUND_NEEDS_ONNX)
option(ENABLE_CUDA "Enable NVIDIA CUDA GPU acceleration" OFF)
option(ENABLE_DIRECTML "Enable DirectML GPU acceleration (Windows)" OFF)

if(ENABLE_CUDA)
    message(STATUS "CUDA GPU acceleration enabled")
    if(WIN32)
        set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v1.17.1/onnxruntime-win-x64-gpu-1.17.1.zip")
    else()
        set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v1.17.1/onnxruntime-linux-x64-gpu-1.17.1.tgz")
    endif()
    add_compile_definitions(USE_CUDA=1)
elseif(ENABLE_DIRECTML AND WIN32)
    message(STATUS "DirectML GPU acceleration enabled")
    set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v1.17.1/onnxruntime-win-x64-1.17.1.zip")
    add_compile_definitions(USE_DIRECTML=1)
elseif(APPLE)
    set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v1.17.1/onnxruntime-osx-universal-1.17.1.tgz")
elseif(WIN32)
    set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v1.17.1/onnxruntime-win-x64-1.17.1.zip")
else()
    set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v1.17.1/onnxruntime-linux-x64-1.17.1.tgz")
endif()

FetchContent_Declare(
    onnxruntime
    URL ${ORT_URL}
)
FetchContent_MakeAvailable(onnxruntime)

if(WIN32)
    set(ORT_LIB_REL_PATH "lib/onnxruntime.lib")
else()
    set(ORT_LIB_REL_PATH "lib/libonnxruntime.so")
endif()

add_library(onnx_runtime_lib SHARED IMPORTED)
set_target_properties(onnx_runtime_lib PROPERTIES
    IMPORTED_LOCATION "${onnxruntime_SOURCE_DIR}/${ORT_LIB_REL_PATH}"
    INTERFACE_INCLUDE_DIRECTORIES "${onnxruntime_SOURCE_DIR}/include"
)

endif()

if(whisper IN_LIST SOUND_SELECTED_EFFECTS)
    find_package(Freetype REQUIRED)
    FetchContent_Declare(json
        GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3)
    FetchContent_Declare(pocketfft
        GIT_REPOSITORY https://github.com/mreineck/pocketfft.git GIT_TAG cpp)
    FetchContent_MakeAvailable(json pocketfft)
    FetchContent_Declare(whisper
        GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
        GIT_TAG master)
    set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(whisper)
    if(UNIX AND NOT APPLE)
        foreach(dependency IN ITEMS whisper ggml ggml-base ggml-cpu)
            if(TARGET ${dependency})
                set_target_properties(${dependency} PROPERTIES
                    BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN")
            endif()
        endforeach()
    endif()
endif()

if(demucs_filter IN_LIST SOUND_SELECTED_EFFECTS OR
   demucs_source IN_LIST SOUND_SELECTED_EFFECTS)
    FetchContent_Declare(eigen
        GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git GIT_TAG 3.4.0)
    FetchContent_Declare(demucs_onnx
        GIT_REPOSITORY https://github.com/sevagh/demucs.onnx.git
        GIT_TAG main GIT_SUBMODULES "")
    FetchContent_MakeAvailable(eigen demucs_onnx)
    set(DEMUCS_ONNX_SRC
        ${demucs_onnx_SOURCE_DIR}/src/dsp.cpp
        ${demucs_onnx_SOURCE_DIR}/src/model_apply.cpp
        ${demucs_onnx_SOURCE_DIR}/src/model_inference.cpp)
endif()
endif() # quarantined-by-default effect frameworks

# =============================================================================
# Plugin Support (Logging)
# =============================================================================
configure_file(src/plugin-support.c.in ${CMAKE_BINARY_DIR}/plugin-support.c)

# =============================================================================
# Build Targets
# =============================================================================
set(PLUGIN_SOURCES
    ${CMAKE_BINARY_DIR}/plugin-support.c
    src/plugin-main.cpp
)
set(SOUND_EFFECT_SOURCE_yamnet src/yamnet-worker-filter.cpp)
set(SOUND_EFFECT_SOURCE_tts src/tts-source.cpp)
set(SOUND_EFFECT_SOURCE_stt src/stt-filter.cpp)
set(SOUND_EFFECT_SOURCE_diffusion src/audio-diffusion-source.cpp)
set(SOUND_EFFECT_SOURCE_ddsp src/ddsp-filter.cpp)
set(SOUND_EFFECT_SOURCE_stem_live src/stem-live-filter.cpp)
set(SOUND_EFFECT_SOURCE_meanvc2_bridge src/meanvc2-bridge-source.cpp)
set(SOUND_EFFECT_SOURCE_rvc src/rvc-native-filter.cpp)
set(SOUND_EFFECT_SOURCE_deepfilter src/deepfilter-filter.cpp)
set(SOUND_EFFECT_SOURCE_demucs_filter src/demucs-filter.cpp)
set(SOUND_EFFECT_SOURCE_demucs_source src/demucs-source.cpp)
set(SOUND_EFFECT_SOURCE_whisper src/whisper-source.cpp src/framework/text_renderer.cpp)
set(SOUND_EFFECT_SOURCE_chatterbox src/chatterbox-source.cpp)
set(SOUND_EFFECT_SOURCE_kokoro src/kokoro-source.cpp)
set(SOUND_EFFECT_SOURCE_matchering src/matchering-filter.cpp)
set(SOUND_EFFECT_SOURCE_musicgen src/musicgen-source.cpp)
set(SOUND_EFFECT_SOURCE_mediapipe src/mediapipe-filter.cpp)
foreach(effect IN LISTS SOUND_SELECTED_EFFECTS)
    list(APPEND PLUGIN_SOURCES ${SOUND_EFFECT_SOURCE_${effect}})
endforeach()
if(SOUND_SELECTED_EFFECTS)
    list(APPEND PLUGIN_SOURCES ${DEMUCS_ONNX_SRC})
endif()
if(SOUND_TOOLS_MENU)
    list(APPEND PLUGIN_SOURCES src/qt-check.cpp src/optimizer_ui.cpp)
endif()

add_library(live-latent-lab-sound MODULE ${PLUGIN_SOURCES})
if(ddsp IN_LIST SOUND_SELECTED_EFFECTS)
    set(SOUND_DDSP_INCLUDE "" CACHE PATH "Pinned LiteRT 2.4.0 C ABI headers")
    if(NOT EXISTS "${SOUND_DDSP_INCLUDE}/litert/c/litert_compiled_model.h")
        message(FATAL_ERROR "DDSP requires pinned LiteRT C API headers")
    endif()
    find_package(OpenSSL REQUIRED)
    target_include_directories(live-latent-lab-sound PRIVATE "${SOUND_DDSP_INCLUDE}")
    target_link_libraries(live-latent-lab-sound PRIVATE OpenSSL::Crypto ${CMAKE_DL_LIBS})
endif()
if(SOUND_SETTINGS_PAGE)
    target_sources(live-latent-lab-sound PRIVATE src/ui/sound_settings.cpp)
    target_compile_definitions(live-latent-lab-sound PRIVATE SOUND_WITH_SETTINGS_PAGE=1)
    target_link_libraries(live-latent-lab-sound PRIVATE Qt6::Widgets)
endif()
if(SOUND_DIFFUSION_DOCK)
    find_package(obs-frontend-api REQUIRED)
    target_sources(live-latent-lab-sound PRIVATE src/ui/diffusion_dock.cpp)
    target_compile_definitions(live-latent-lab-sound PRIVATE SOUND_WITH_DIFFUSION_DOCK=1)
    target_link_libraries(live-latent-lab-sound PRIVATE Qt6::Widgets OBS::obs-frontend-api)
endif()

if(stt IN_LIST SOUND_SELECTED_EFFECTS)
    if(NOT (UNIX AND NOT APPLE))
        message(FATAL_ERROR "Sound STT currently supports Linux only")
    endif()
    set(SOUND_STT_WHISPER_INCLUDE "" CACHE PATH "Matched whisper.cpp C API include directory")
    set(SOUND_STT_RUNTIME_DIR "" CACHE PATH "Qualified Whisper CPU shared libraries with sibling RUNPATH")
    set(SOUND_STT_NEMOTRON_RUNTIME_DIR "" CACHE PATH "Qualified isolated Nemotron runtime and license notices")
    if(SOUND_STT_NEMOTRON_RUNTIME_DIR)
        foreach(asset libsound-nemotron-runtime.so NEMOTRON-TRANSCRIBE-LICENSE NEMOTRON-GGML-LICENSE NEMOTRON-THIRD-PARTY-LICENSES)
            if(NOT EXISTS "${SOUND_STT_NEMOTRON_RUNTIME_DIR}/${asset}")
                message(FATAL_ERROR "Missing Nemotron runtime asset: ${asset}")
            endif()
        endforeach()
    endif()

    if(NOT EXISTS "${SOUND_STT_WHISPER_INCLUDE}/whisper.h" OR NOT EXISTS "${SOUND_STT_RUNTIME_DIR}/libwhisper.so.1")
        message(FATAL_ERROR "STT requires explicit matched Whisper headers and runtime")
    endif()
    target_include_directories(live-latent-lab-sound PRIVATE "${SOUND_STT_WHISPER_INCLUDE}")
    target_link_libraries(live-latent-lab-sound PRIVATE ${CMAKE_DL_LIBS})
endif()

set(SOUND_ZIPFORMER_RUNTIME_DIR "" CACHE PATH "Optional pinned Sherpa-ONNX CPU runtime")
if(stt IN_LIST SOUND_SELECTED_EFFECTS AND SOUND_ZIPFORMER_RUNTIME_DIR)
    foreach(asset c-api.h libsound-zipformer-sherpa.so libsound-zipformer-onnxruntime.so SHERPA-LICENSE ONNXRUNTIME-LICENSE ONNXRUNTIME-THIRDPARTYNOTICES.TXT SHA256SUMS)
        if(NOT EXISTS "${SOUND_ZIPFORMER_RUNTIME_DIR}/${asset}")
            message(FATAL_ERROR "Missing Zipformer runtime asset: ${asset}")
        endif()
    endforeach()
    file(SHA256 "${SOUND_ZIPFORMER_RUNTIME_DIR}/c-api.h" SOUND_ZIPFORMER_HEADER_HASH)
    if(NOT SOUND_ZIPFORMER_HEADER_HASH STREQUAL "437b1279047877167d8fadc74a60d47f3df514d703fdac1c1b6851da9bc2fdb4")
        message(FATAL_ERROR "Unqualified Sherpa-ONNX ABI header")
    endif()
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(live-latent-lab-sound PRIVATE SOUND_WITH_ZIPFORMER)
    target_include_directories(live-latent-lab-sound PRIVATE "${SOUND_ZIPFORMER_RUNTIME_DIR}")
    target_link_libraries(live-latent-lab-sound PRIVATE OpenSSL::Crypto)
endif()

if(diffusion IN_LIST SOUND_SELECTED_EFFECTS)
    if(NOT (UNIX AND NOT APPLE))
        message(FATAL_ERROR "Sound Audio Diffusion currently supports Linux only")
    endif()
    set(SOUND_SA3_RUNTIME_DIR "" CACHE PATH "Qualified SA3 runtime with private GGML libraries")
    if(NOT EXISTS "${SOUND_SA3_RUNTIME_DIR}/libsa3.so" OR NOT EXISTS "${SOUND_SA3_RUNTIME_DIR}/libsa3_v1.h"
       OR NOT EXISTS "${SOUND_SA3_RUNTIME_DIR}/LICENSE-sa3.txt" OR NOT EXISTS "${SOUND_SA3_RUNTIME_DIR}/LICENSE-ggml.txt")
        message(FATAL_ERROR "Audio Diffusion requires the qualified SA3 runtime, header and licenses")
    endif()
    file(SHA256 "${SOUND_SA3_RUNTIME_DIR}/libsa3_v1.h" SOUND_SA3_HEADER_HASH)
    if(NOT SOUND_SA3_HEADER_HASH STREQUAL "d4eb236988c978393dac3a7b6470049720dcb3e5dee57a4800d31b5d213342e6")
        message(FATAL_ERROR "Unqualified SA3 ABI header")
    endif()
    target_include_directories(live-latent-lab-sound PRIVATE "${SOUND_SA3_RUNTIME_DIR}")
    target_link_libraries(live-latent-lab-sound PRIVATE ${CMAKE_DL_LIBS})
    set(SOUND_SAME_SOURCE_DIR "" CACHE PATH "Pinned sa3.cpp checkout for isolated SAME-S morph helper")
    if(SOUND_SAME_SOURCE_DIR)
        foreach(pair "${SOUND_SAME_SOURCE_DIR}|28ebf5f5337ba6f36966db76d16cad1fc5ed2b70" "${SOUND_SAME_SOURCE_DIR}/ggml|a373144b892cb1ef7f0c3453efd558caebb0a891")
            string(REPLACE "|" ";" parts "${pair}")
            list(GET parts 0 checkout)
            list(GET parts 1 expected)
            execute_process(COMMAND git -C "${checkout}" rev-parse HEAD OUTPUT_VARIABLE revision OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE result)
            execute_process(COMMAND git -C "${checkout}" status --porcelain --untracked-files=no OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE status_result)
            if(NOT result EQUAL 0 OR NOT status_result EQUAL 0 OR NOT revision STREQUAL expected OR NOT dirty STREQUAL "")
                message(FATAL_ERROR "SAME-S requires clean pinned upstream sources")
            endif()
        endforeach()
        find_package(OpenSSL REQUIRED)
        set(same_ggml "${SOUND_SA3_RUNTIME_DIR}/libsa3-cuda-a373144b-ggml.so.0.29.0")
        set(same_base "${SOUND_SA3_RUNTIME_DIR}/libsa3-cuda-a373144b-ggml-base.so.0.29.0")
        if(NOT EXISTS "${same_ggml}" OR NOT EXISTS "${same_base}")
            message(FATAL_ERROR "SAME-S requires the qualified private GGML runtime")
        endif()
        add_executable(sound-sa3-morph tools/sa3-latent-morph.cpp)
        target_compile_features(sound-sa3-morph PRIVATE cxx_std_17)
        target_include_directories(sound-sa3-morph PRIVATE "${SOUND_SAME_SOURCE_DIR}/src" "${SOUND_SAME_SOURCE_DIR}/ggml/include")
        target_link_libraries(sound-sa3-morph PRIVATE "${same_ggml}" "${same_base}" OpenSSL::Crypto)
        set_target_properties(sound-sa3-morph PROPERTIES INSTALL_RPATH "$ORIGIN")
        target_compile_definitions(live-latent-lab-sound PRIVATE SOUND_WITH_SAME_MORPH=1)
    endif()
endif()

if(tts IN_LIST SOUND_SELECTED_EFFECTS)
    if(NOT (UNIX AND NOT APPLE))
        message(FATAL_ERROR "Sound TTS workers currently support Linux only")
    endif()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SOUND_CURL REQUIRED IMPORTED_TARGET libcurl)
    pkg_check_modules(SOUND_TTS_AV REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswresample)
    target_link_libraries(live-latent-lab-sound PRIVATE PkgConfig::SOUND_CURL PkgConfig::SOUND_TTS_AV)
endif()
foreach(effect IN LISTS SOUND_SELECTED_EFFECTS)
    string(TOUPPER "${effect}" effect_define)
    target_compile_definitions(live-latent-lab-sound PRIVATE SOUND_WITH_${effect_define}=1)
endforeach()
if(SOUND_TOOLS_MENU)
    target_compile_definitions(live-latent-lab-sound PRIVATE SOUND_WITH_TOOLS_MENU=1)
endif()

# Unique log tokens identify exact candidate builds.
if(stem_live IN_LIST SOUND_SELECTED_EFFECTS AND NOT SOUND_TOOLS_MENU)
    string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef SOUND_STEM_BUILD_NONCE)
    target_compile_definitions(live-latent-lab-sound PRIVATE
        "SOUND_STEM_BUILD_MARKER=\"SOUND_STEM_LIVE_CANDIDATE_${SOUND_STEM_BUILD_NONCE}\"")
    file(WRITE "${CMAKE_BINARY_DIR}/sound-stem-build-nonce.txt" "${SOUND_STEM_BUILD_NONCE}\n")
endif()
if(SOUND_SELECTED_EFFECTS STREQUAL "yamnet" AND NOT SOUND_TOOLS_MENU)
    string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef SOUND_YAMNET_BUILD_NONCE)
    target_compile_definitions(live-latent-lab-sound PRIVATE
        "SOUND_YAMNET_BUILD_MARKER=\"SOUND_YAMNET_ASYNC_CANDIDATE_${SOUND_YAMNET_BUILD_NONCE}\"")
    file(WRITE "${CMAKE_BINARY_DIR}/sound-yamnet-build-nonce.txt"
        "${SOUND_YAMNET_BUILD_NONCE}\n")
endif()

# Check if LibOBS_LIBRARY is set (it might be empty if we just fetched headers)
if(LibOBS_LIBRARY)
    target_link_libraries(live-latent-lab-sound PRIVATE ${LibOBS_LIBRARY})
else()
    # If no library, we rely on dynamic lookup (Linux/macOS)
    if(APPLE)
        set_target_properties(live-latent-lab-sound PROPERTIES LINK_FLAGS "-undefined dynamic_lookup")
    elseif(UNIX AND NOT WIN32)
        set_target_properties(live-latent-lab-sound PROPERTIES LINK_FLAGS "-Wl,-undefined,dynamic_lookup")
    endif()
endif()

target_include_directories(live-latent-lab-sound PRIVATE
    src
    ${LibOBS_INCLUDE_DIR}
)
if(SOUND_NEEDS_ONNX)
    target_link_libraries(live-latent-lab-sound PRIVATE onnx_runtime_lib)
endif()
if(whisper IN_LIST SOUND_SELECTED_EFFECTS)
    target_link_libraries(live-latent-lab-sound PRIVATE
        Freetype::Freetype whisper nlohmann_json::nlohmann_json)
    target_include_directories(live-latent-lab-sound PRIVATE
        ${pocketfft_SOURCE_DIR})
endif()
if(demucs_filter IN_LIST SOUND_SELECTED_EFFECTS OR
   demucs_source IN_LIST SOUND_SELECTED_EFFECTS)
    target_link_libraries(live-latent-lab-sound PRIVATE Eigen3::Eigen)
    target_include_directories(live-latent-lab-sound PRIVATE
        ${demucs_onnx_SOURCE_DIR}/src ${eigen_SOURCE_DIR})
endif()
if(SOUND_SELECTED_EFFECTS)
    target_link_libraries(live-latent-lab-sound PRIVATE pthread)
endif()
if(SOUND_TOOLS_MENU)
    target_link_libraries(live-latent-lab-sound PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets)
endif()

# Properties
set_target_properties(live-latent-lab-sound PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    INSTALL_RPATH "$ORIGIN"
    BUILD_WITH_INSTALL_RPATH TRUE
)

if(WIN32)
    set_target_properties(live-latent-lab-sound PROPERTIES SUFFIX ".dll")
elseif(APPLE)
    set_target_properties(live-latent-lab-sound PROPERTIES SUFFIX ".so" BUNDLE FALSE)
else()
    set_target_properties(live-latent-lab-sound PROPERTIES SUFFIX ".so")
endif()

# =============================================================================
# Installation
# =============================================================================
include(GNUInstallDirs)
if(NOT DEFINED OBS_PLUGIN_DESTINATION)
    set(OBS_PLUGIN_DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
endif()
if(NOT DEFINED OBS_DATA_DESTINATION)
    set(OBS_DATA_DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/${CMAKE_PROJECT_NAME}")
endif()

install(TARGETS live-latent-lab-sound LIBRARY DESTINATION ${OBS_PLUGIN_DESTINATION})
# Package only maintained runtime resources, never ignored local assets.
install(FILES data/manifest.json DESTINATION "${OBS_DATA_DESTINATION}")
install(FILES data/locale/en-US.ini DESTINATION "${OBS_DATA_DESTINATION}/locale")

if(SOUND_NEEDS_ONNX AND UNIX AND NOT APPLE)
    install(DIRECTORY "${onnxruntime_SOURCE_DIR}/lib/"
        DESTINATION "${OBS_PLUGIN_DESTINATION}"
        FILES_MATCHING PATTERN "libonnxruntime.so*")
endif()
if(whisper IN_LIST SOUND_SELECTED_EFFECTS AND UNIX AND NOT APPLE)
    install(DIRECTORY "${CMAKE_BINARY_DIR}/bin/"
        DESTINATION "${OBS_PLUGIN_DESTINATION}"
        FILES_MATCHING
            PATTERN "libwhisper.so*"
            PATTERN "libggml.so*"
            PATTERN "libggml-base.so*"
            PATTERN "libggml-cpu.so*")
endif()

if(stt IN_LIST SOUND_SELECTED_EFFECTS)
    install(DIRECTORY "${SOUND_STT_RUNTIME_DIR}/" DESTINATION "${OBS_PLUGIN_DESTINATION}/stt-runtime"
        FILES_MATCHING PATTERN "libwhisper.so*" PATTERN "libggml.so*" PATTERN "libggml-base.so*" PATTERN "libggml-cpu.so*" PATTERN "LICENSE")
    if(SOUND_STT_NEMOTRON_RUNTIME_DIR)
        install(DIRECTORY "${SOUND_STT_NEMOTRON_RUNTIME_DIR}/" DESTINATION "${OBS_PLUGIN_DESTINATION}/stt-runtime"
            FILES_MATCHING PATTERN "libsound-nemotron-runtime.so" PATTERN "NEMOTRON-*-LICENSE" PATTERN "NEMOTRON-THIRD-PARTY-LICENSES")
    endif()

endif()


if(diffusion IN_LIST SOUND_SELECTED_EFFECTS)
    if(TARGET sound-sa3-morph)
        install(TARGETS sound-sa3-morph RUNTIME DESTINATION "${OBS_PLUGIN_DESTINATION}/sa3-runtime")
    endif()
    install(DIRECTORY "${SOUND_SA3_RUNTIME_DIR}/" DESTINATION "${OBS_PLUGIN_DESTINATION}/sa3-runtime"
        FILES_MATCHING PATTERN "libsa3*.so*" PATTERN "LICENSE*.txt" PATTERN "THIRD_PARTY_NOTICES.md" PATTERN "SHA256SUMS")
endif()

if(stt IN_LIST SOUND_SELECTED_EFFECTS AND SOUND_ZIPFORMER_RUNTIME_DIR)
    install(DIRECTORY "${SOUND_ZIPFORMER_RUNTIME_DIR}/" DESTINATION "${OBS_PLUGIN_DESTINATION}/stt-runtime/zipformer"
        FILES_MATCHING PATTERN "libsound-zipformer-*.so" PATTERN "*LICENSE*" PATTERN "*NOTICES*" PATTERN "SHA256SUMS")
endif()
