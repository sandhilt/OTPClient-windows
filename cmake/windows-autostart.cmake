# Include once from windows.cmake; the extra deferred call runs after its
# compatibility wiring and OTP_WINDOWS_SHIM_CHECKS option are available.
include_guard(GLOBAL)
if(NOT WIN32)
    return()
endif()

function(otp_windows_autostart_apply target)
    if(NOT WIN32)
        return()
    endif()
    get_target_property(_applied ${target} OTP_WINDOWS_AUTOSTART_APPLIED)
    if(_applied)
        return()
    endif()
    set(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/platform/windows")
    get_target_property(_sources ${target} SOURCES)
    set(_linux "${PROJECT_SOURCE_DIR}/src/gui/autostart.c")
    if(NOT _linux IN_LIST _sources)
        message(FATAL_ERROR "${target}: expected Linux autostart source was not found")
    endif()
    list(REMOVE_ITEM _sources "${_linux}")
    set_property(TARGET ${target} PROPERTY SOURCES "${_sources}")
    target_sources(${target} PRIVATE
        "${_root}/autostart-win.c" "${_root}/autostart-win-registry.c")
    set_source_files_properties("${_root}/autostart-win.c" "${_root}/autostart-win-registry.c"
        TARGET_DIRECTORY ${target} PROPERTIES COMPILE_DEFINITIONS OTP_WINDOWS_COMPAT_IMPLEMENTATION)
    target_link_libraries(${target} advapi32)
    set_property(TARGET ${target} PROPERTY OTP_WINDOWS_AUTOSTART_APPLIED TRUE)
endfunction()

function(otp_windows_autostart_targets)
    if(BUILD_GUI AND TARGET ${PROJECT_NAME})
        otp_windows_autostart_apply(${PROJECT_NAME})
    endif()
    if(OTP_WINDOWS_SHIM_CHECKS)
        set(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/platform/windows")
        add_executable(otp-windows-autostart-check
            "${_root}/autostart-win-check.c" "${_root}/autostart-win-registry.c")
        target_link_libraries(otp-windows-autostart-check PRIVATE PkgConfig::GIO advapi32)
        target_compile_options(otp-windows-autostart-check PRIVATE
            -Wall -Wextra -Werror=format-security -Werror=implicit-function-declaration)
        set_target_properties(otp-windows-autostart-check PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
        # Upstream BUILD_TESTING is OFF on Windows. Invoke this GTest executable
        # directly; do not enable/register the upstream D-Bus/GUI test suite.
    endif()
endfunction()
cmake_language(DEFER CALL otp_windows_autostart_targets)
