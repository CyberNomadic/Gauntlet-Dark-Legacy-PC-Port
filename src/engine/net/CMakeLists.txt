find_program(GDL_CARGO cargo REQUIRED)
find_package(Threads REQUIRED)

# Registry, intermediates and temporaries stay in the build tree. Offline builds
# need no Rust toolchain; the lockfile pins all networking dependencies.
if(WIN32)
    if(GDL_WINDOWS_STATIC_RUNTIME)
        if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
            message(FATAL_ERROR "The static Windows netplay bridge requires native x64 MSVC")
        endif()
        set(GDL_IROH_TARGET x86_64-pc-windows-msvc)
        set(GDL_IROH_TARGET_ARGS --target ${GDL_IROH_TARGET})
        set(GDL_IROH_LIBRARY
            "${CMAKE_CURRENT_BINARY_DIR}/cargo/${GDL_IROH_TARGET}/release/gdl_iroh.lib")
    else()
        set(GDL_IROH_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/cargo/release/gdl_iroh.lib")
    endif()
    set(GDL_IROH_SYSTEM_LIBRARIES ws2_32 bcrypt userenv ntdll secur32 crypt32 advapi32 iphlpapi)
else()
    set(GDL_IROH_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/cargo/release/libgdl_iroh.a")
    set(GDL_IROH_SYSTEM_LIBRARIES ${CMAKE_DL_LIBS} m)
endif()
if(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "The netplay Rust bridge currently requires a native target build")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tmp")
set(GDL_CARGO_HOME "${CMAKE_SOURCE_DIR}/build/cargo-home" CACHE PATH "Networking dependency cache")
set(GDL_IROH_ENV "CARGO_HOME=${GDL_CARGO_HOME}"
    "CARGO_TARGET_DIR=${CMAKE_CURRENT_BINARY_DIR}/cargo"
    "TEMP=${CMAKE_CURRENT_BINARY_DIR}/tmp" "TMP=${CMAKE_CURRENT_BINARY_DIR}/tmp"
    "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/tmp")
if(GDL_WINDOWS_STATIC_RUNTIME)
    # cc-rs (including ring's C/assembly build) reads Cargo's crt-static target
    # feature and selects /MT too. The explicit target above leaves proc-macro
    # and build-script executables on their ordinary host runtime.
    gdl_static_rust_flags(GDL_IROH_RUSTFLAGS)
    list(APPEND GDL_IROH_ENV "CARGO_ENCODED_RUSTFLAGS=${GDL_IROH_RUSTFLAGS}")
endif()
add_custom_command(OUTPUT "${GDL_IROH_LIBRARY}"
    COMMAND ${CMAKE_COMMAND} -E env ${GDL_IROH_ENV} ${GDL_CARGO} build --release --locked
        ${GDL_IROH_TARGET_ARGS}
        --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/iroh/Cargo.toml" --jobs 2
    DEPENDS iroh/Cargo.toml iroh/Cargo.lock iroh/src/lib.rs CMakeLists.txt
        "${CMAKE_SOURCE_DIR}/cmake/GdlStaticRuntime.cmake"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" VERBATIM)
add_custom_target(iroh_bridge_build DEPENDS "${GDL_IROH_LIBRARY}")
add_library(iroh_bridge STATIC IMPORTED GLOBAL)
set_target_properties(iroh_bridge PROPERTIES IMPORTED_LOCATION "${GDL_IROH_LIBRARY}")
add_dependencies(iroh_bridge iroh_bridge_build)

add_library(engine_net STATIC IrohTransport.cpp)
add_library(gdl::engine_net ALIAS engine_net)
target_link_libraries(engine_net PUBLIC gdl::engine_base
    PRIVATE iroh_bridge Threads::Threads ${GDL_IROH_SYSTEM_LIBRARIES})
gdl_apply_compiler_options(engine_net)
set_target_properties(engine_net PROPERTIES FOLDER "engine")
