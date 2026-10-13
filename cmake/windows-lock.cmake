# Included by cmake/windows.cmake; no upstream source/build edits are needed.
include_guard(GLOBAL)

function(otp_windows_lock_apply target)
    if(NOT WIN32)
        message(FATAL_ERROR "The Windows lock source replacement requires WIN32")
    endif()
    set(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/platform/windows")
    get_filename_component(_linux "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/gui/lock-app.c" ABSOLUTE)
    get_target_property(_sources ${target} SOURCES)
    set(_matches ${_sources})
    list(FILTER _matches INCLUDE REGEX "(^|/)lock-app\\.c$")
    list(LENGTH _matches _count)
    if(NOT _count EQUAL 1 OR NOT _linux IN_LIST _sources)
        message(FATAL_ERROR "${target}: expected exactly one absolute src/gui/lock-app.c source")
    endif()
    list(REMOVE_ITEM _sources "${_linux}")
    set_property(TARGET ${target} PROPERTY SOURCES "${_sources}")
    target_sources(${target} PRIVATE
        "${_root}/lock-app-win.c" "${_root}/lock-events.c" "${_root}/lock-policy.c")
    # Win8+ declarations; the port's native target is Windows 10/11 x64.
    # Source-local: do not change the upstream application's Win32 API level.
    set_source_files_properties("${_root}/lock-events.c"
        TARGET_DIRECTORY ${target} PROPERTIES
        COMPILE_DEFINITIONS "OTP_WINDOWS_COMPAT_IMPLEMENTATION;_WIN32_WINNT=0x0602")
    # Match the upstream target's plain target_link_libraries signature.
    target_link_libraries(${target} user32 wtsapi32)
endfunction()

function(otp_windows_lock_targets)
    if(NOT WIN32)
        return()
    endif()
    if(BUILD_GUI)
        otp_windows_lock_apply(OTPClient)
    endif()
    if(OTP_WINDOWS_SHIM_CHECKS)
        set(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/platform/windows")
        add_executable(otp-windows-lock-check
            "${_root}/lock-check.c" "${_root}/lock-policy.c" "${_root}/lock-events.c")
        target_compile_features(otp-windows-lock-check PRIVATE c_std_11)
        target_compile_definitions(otp-windows-lock-check PRIVATE _WIN32_WINNT=0x0602)
        target_compile_options(otp-windows-lock-check PRIVATE
            -Wall -Wextra -Werror=format-security -Werror=implicit-function-declaration)
        target_link_libraries(otp-windows-lock-check PRIVATE PkgConfig::GLIB2 user32 wtsapi32)
        # BUILD_TESTING remains OFF: build a standalone GTest executable,
        # like the existing Windows checks; run it explicitly on Windows.
        add_executable(otp-windows-lock-probe
            "${_root}/lock-probe.c" "${_root}/lock-events.c" "${_root}/lock-policy.c")
        target_compile_features(otp-windows-lock-probe PRIVATE c_std_11)
        target_compile_definitions(otp-windows-lock-probe PRIVATE _WIN32_WINNT=0x0602)
        target_compile_options(otp-windows-lock-probe PRIVATE
            -Wall -Wextra -Werror=format-security -Werror=implicit-function-declaration)
        target_link_libraries(otp-windows-lock-probe PRIVATE PkgConfig::GLIB2 user32 wtsapi32)
    endif()
endfunction()

# Targets have not been defined when the project hook is included. The
# deferred call works whether this include precedes or follows the common
# compatibility hook's own DEFER: no dependency on its target mutations.
cmake_language(DEFER CALL otp_windows_lock_targets)
