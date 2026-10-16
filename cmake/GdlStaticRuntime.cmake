# The shipping Windows preset pins the simulation CRT in the executable instead
# of the Windows-serviced UCRT DLL. Dynamic-CRT developer profiles stay unchanged.
# Validate before project() so compiler probes and vcpkg see the same contract.
function(gdl_configure_windows_runtime)
    if(NOT CMAKE_HOST_WIN32 OR
       (DEFINED CMAKE_SYSTEM_NAME AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows"))
        return()
    endif()
    if(NOT VCPKG_TARGET_TRIPLET STREQUAL "x64-windows-static" AND
       (NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY OR CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL"))
        return()
    endif()
    if(NOT CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreaded")
        message(FATAL_ERROR
            "The static Windows shipping profile requires CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded. "
            "Use a fresh build directory; do not mix /MD objects with /MT dependencies.")
    endif()
    if(NOT VCPKG_TARGET_TRIPLET STREQUAL "x64-windows-static")
        message(FATAL_ERROR
            "The static Windows shipping profile requires VCPKG_TARGET_TRIPLET=x64-windows-static. "
            "Configure a fresh build directory instead of reusing a dynamic-CRT cache.")
    endif()
    if(CMAKE_CONFIGURATION_TYPES OR CMAKE_GENERATOR MATCHES "Visual Studio|Multi-Config|Xcode" OR
       NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
        message(FATAL_ERROR
            "The static Windows CRT profile currently supports single-config Release/RelWithDebInfo only. "
            "Use the unchanged dynamic-CRT Debug/Visual Studio presets for development.")
    endif()
    set(GDL_WINDOWS_STATIC_RUNTIME ON PARENT_SCOPE)
endfunction()

# Reject explicit flag overrides after compiler/toolchain initialization as well.
function(gdl_validate_windows_runtime)
    if(NOT GDL_WINDOWS_STATIC_RUNTIME)
        return()
    endif()
    if(NOT MSVC OR NOT CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreaded" OR
       NOT VCPKG_TARGET_TRIPLET STREQUAL "x64-windows-static")
        message(FATAL_ERROR "Windows objects and dependencies must use the pinned static CRT")
    endif()
    foreach(_flags IN ITEMS CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
                            CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_MINSIZEREL)
        if("${${_flags}}" MATCHES "(^|[ \t])[-/](MDd?|MTd)($|[ \t])")
            message(FATAL_ERROR "${_flags} overrides the pinned static CRT with /MD or /MTd")
        endif()
    endforeach()
    foreach(_environment IN ITEMS CL _CL_ CXXFLAGS)
        if("$ENV{${_environment}}" MATCHES "(^|[ \t])[-/](MDd?|MTd)($|[ \t])")
            message(FATAL_ERROR "${_environment} overrides the pinned static CRT with /MD or /MTd")
        endif()
    endforeach()
endfunction()

# Cargo's encoded form has precedence over RUSTFLAGS/config flags. Preserve the
# caller's effective flags and append the target CRT requirement explicitly.
# An explicit --target keeps these flags off host-side proc-macro/build-script DLLs.
function(gdl_static_rust_flags output)
    foreach(_environment IN ITEMS CFLAGS TARGET_CFLAGS CFLAGS_x86_64_pc_windows_msvc
                                  CFLAGS_x86_64-pc-windows-msvc CXXFLAGS TARGET_CXXFLAGS
                                  CXXFLAGS_x86_64_pc_windows_msvc CXXFLAGS_x86_64-pc-windows-msvc)
        if("$ENV{${_environment}}" MATCHES "(^|[ \t])[-/](MDd?|MTd)($|[ \t])")
            message(FATAL_ERROR "${_environment} overrides the Rust bridge's release static CRT")
        endif()
    endforeach()
    string(ASCII 31 _separator)
    if(DEFINED ENV{CARGO_ENCODED_RUSTFLAGS})
        set(_flags "$ENV{CARGO_ENCODED_RUSTFLAGS}")
    else()
        separate_arguments(_arguments NATIVE_COMMAND "$ENV{RUSTFLAGS}")
        list(JOIN _arguments "${_separator}" _flags)
    endif()
    if(_flags MATCHES "-crt-static")
        message(FATAL_ERROR "Rust flags explicitly disable the pinned static CRT")
    endif()
    if(NOT _flags STREQUAL "")
        string(APPEND _flags "${_separator}")
    endif()
    string(APPEND _flags "-C${_separator}target-feature=+crt-static")
    set(${output} "${_flags}" PARENT_SCOPE)
endfunction()
