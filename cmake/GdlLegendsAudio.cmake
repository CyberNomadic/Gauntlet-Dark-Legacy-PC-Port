include_guard(GLOBAL)
include(FetchContent)
include("${CMAKE_CURRENT_LIST_DIR}/GdlPatchAmuse.cmake")

# Native MusyX synthesis only. The upstream tools/editor, archive converters,
# Boo audio backend and FluidSynth export path are deliberately not built.
function(gdl_add_legends_audio)
    if(TARGET gdl_legends_audio_native)
        return()
    endif()

    # Function scope keeps dependency testing/install options out of the parent.
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING OFF)
    set(FMT_TEST OFF)
    set(FMT_DOC OFF)
    set(FMT_INSTALL OFF)
    set(NOWIDE_INSTALL OFF)
    set(NOWIDE_WERROR OFF)

    FetchContent_Declare(gdl_legends_fmt
        URL https://codeload.github.com/fmtlib/fmt/tar.gz/refs/tags/11.1.4
        URL_HASH SHA256=ac366b7b4c2e9f0dde63a59b3feb5ee59b67974b14ee5dc9ea8ad78aa2c1ee1e
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        EXCLUDE_FROM_ALL)
    FetchContent_Declare(gdl_legends_nowide
        URL https://codeload.github.com/boostorg/nowide/tar.gz/a5f301017aad445b663c14e12960c1f3626c2324
        URL_HASH SHA256=445810f332c475e27239b6e3b6c67a685cef3cb5d03e92b4bfbf1e6e8f36bef8
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        EXCLUDE_FROM_ALL)
    FetchContent_Declare(gdl_legends_amuse
        URL https://codeload.github.com/derselbst/amuse/tar.gz/503ad90e9857584f23d3389982f4883fa3190a5d
        URL_HASH SHA256=427846e3c3f9e81a4efc5b921a2cbf96ec8b1d4cd43284f802a6bd36f6d56bcc
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        SOURCE_SUBDIR _gdl_native_engine_only
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(gdl_legends_fmt gdl_legends_nowide gdl_legends_amuse)
    gdl_patch_amuse("${gdl_legends_amuse_SOURCE_DIR}")

    set(amuse_sources
        AudioGroup.cpp AudioGroupData.cpp AudioGroupPool.cpp AudioGroupProject.cpp
        AudioGroupSampleDirectory.cpp Common.cpp DirectoryEnumerator.cpp DSPCodec.cpp
        EffectChorus.cpp EffectDelay.cpp EffectReverb.cpp Emitter.cpp Engine.cpp
        Envelope.cpp Listener.cpp N64MusyXCodec.cpp Sequencer.cpp SongState.cpp
        SoundMacroState.cpp Studio.cpp Submix.cpp Voice.cpp VolumeTable.cpp
        atdna_AudioGroupPool.cpp atdna_AudioGroupProject.cpp atdna_AudioGroupSampleDirectory.cpp)
    list(TRANSFORM amuse_sources PREPEND "${gdl_legends_amuse_SOURCE_DIR}/lib/")
    add_library(gdl_legends_audio_native STATIC ${amuse_sources})
    add_library(gdl::legends_audio_native ALIAS gdl_legends_audio_native)
    target_include_directories(gdl_legends_audio_native SYSTEM PUBLIC
        "${gdl_legends_amuse_SOURCE_DIR}/include")
    target_link_libraries(gdl_legends_audio_native PUBLIC fmt::fmt nowide::nowide)
    set_target_properties(gdl_legends_audio_native PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON FOLDER "dependencies")
    if(MSVC)
        target_compile_options(gdl_legends_audio_native PRIVATE
            /bigobj /EHsc /permissive- /Zc:__cplusplus /utf-8)
        target_compile_definitions(gdl_legends_audio_native PRIVATE
            _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    else()
        target_compile_options(gdl_legends_audio_native PRIVATE -Wno-narrowing)
    endif()
    if(WIN32)
        if(MSVC)
            target_compile_options(gdl_legends_audio_native PRIVATE
                "/FI${CMAKE_CURRENT_FUNCTION_LIST_DIR}/GdlAmuseWindows.h")
        else()
            target_compile_options(gdl_legends_audio_native PRIVATE
                -include "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/GdlAmuseWindows.h")
        endif()
    endif()
endfunction()

# Apply the upstream header prerequisite only to the adapter that includes
# Amuse, not transitively to the engine/game or unrelated third-party targets.
function(gdl_configure_legends_audio_source source)
    if(WIN32)
        if(MSVC)
            set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS
                "/FI${CMAKE_CURRENT_FUNCTION_LIST_DIR}/GdlAmuseWindows.h")
        else()
            set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS
                -include "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/GdlAmuseWindows.h")
        endif()
    endif()
endfunction()
