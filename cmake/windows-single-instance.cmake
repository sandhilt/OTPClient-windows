# Include after windows.cmake so its deferred target hook runs first.
include_guard(GLOBAL)

function(otp_windows_single_instance_apply target)
    if(NOT WIN32 OR NOT BUILD_GUI OR NOT "${target}" STREQUAL "${PROJECT_NAME}")
        message(FATAL_ERROR "Single-instance integration requires the Windows GUI target")
    endif()
    get_target_property(_applied ${target} OTP_WINDOWS_SINGLE_INSTANCE_APPLIED)
    if(_applied)
        return()
    endif()
    set(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/platform/windows")
    target_sources(${target} PRIVATE "${_root}/single-instance.c")
    set_source_files_properties("${_root}/single-instance.c" TARGET_DIRECTORY ${target}
        PROPERTIES COMPILE_DEFINITIONS OTP_WINDOWS_COMPAT_IMPLEMENTATION)
    # windows.cmake uses the plain link signature on application targets.
    target_link_libraries(${target} user32 advapi32)
    set_property(TARGET ${target} PROPERTY OTP_WINDOWS_SINGLE_INSTANCE_APPLIED TRUE)
endfunction()

function(otp_windows_single_instance_targets)
    if(NOT WIN32)
        return()
    endif()
    if(BUILD_GUI AND TARGET ${PROJECT_NAME})
        otp_windows_single_instance_apply(${PROJECT_NAME})
    endif()
    if(OTP_WINDOWS_SHIM_CHECKS)
        set(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/platform/windows")
        add_executable(otp-windows-single-instance-check
            "${_root}/single-instance-check.c" "${_root}/single-instance.c")
        target_compile_options(otp-windows-single-instance-check PRIVATE
            -Wall -Wextra -Werror=format-security -Werror=implicit-function-declaration)
        target_compile_definitions(otp-windows-single-instance-check PRIVATE OTP_WINDOWS_COMPAT_IMPLEMENTATION)
        target_link_libraries(otp-windows-single-instance-check PRIVATE PkgConfig::GIO user32 advapi32)
    endif()
endfunction()

cmake_language(DEFER CALL otp_windows_single_instance_targets)
