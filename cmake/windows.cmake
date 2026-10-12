# Loaded by the Windows toolchain or CMAKE_PROJECT_INCLUDE, after project(). Upstream targets and
# source globs stay intact; attach compatibility after all targets exist.
if(NOT WIN32)
    message(FATAL_ERROR "The Windows compatibility layer requires WIN32")
endif()
if(NOT PROJECT_NAME STREQUAL "OTPClient")
    return()
endif()
set(_otp_windows_root "${CMAKE_CURRENT_LIST_DIR}/../src/platform/windows")
include_directories(BEFORE "${_otp_windows_root}/compat")

function(otp_windows_apply_compat _target _header)
    get_target_property(_compile ${_target} COMPILE_OPTIONS)
    list(REMOVE_ITEM _compile -fPIE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3)
    set_property(TARGET ${_target} PROPERTY COMPILE_OPTIONS "${_compile}")
    get_target_property(_link ${_target} LINK_OPTIONS)
    if(_link)
        list(REMOVE_ITEM _link -pie)
        set_property(TARGET ${_target} PROPERTY LINK_OPTIONS "${_link}")
    endif()
    target_sources(${_target} PRIVATE "${_otp_windows_root}/compat.c")
    target_sources(${_target} PRIVATE "${_otp_windows_root}/console.c")
    target_sources(${_target} PRIVATE "${_otp_windows_root}/security.c" "${_otp_windows_root}/private-file.c")
    set_source_files_properties("${_otp_windows_root}/compat.c" "${_otp_windows_root}/console.c" "${_otp_windows_root}/security.c" "${_otp_windows_root}/private-file.c" TARGET_DIRECTORY ${_target} PROPERTIES
        COMPILE_DEFINITIONS OTP_WINDOWS_COMPAT_IMPLEMENTATION)
    target_include_directories(${_target} BEFORE PRIVATE
        "${_otp_windows_root}/compat" "${OTP_WINDOWS_GETTEXT_INCLUDE}")
    target_compile_options(${_target} PRIVATE
        "SHELL:-include \"${_otp_windows_root}/compat/${_header}\"")
    target_link_libraries(${_target} "${OTP_WINDOWS_GETTEXT_LIBRARY}" advapi32)
endfunction()

function(otp_windows_targets)
    if(BUILD_SEARCH_PROVIDER OR BUILD_TESTING OR IS_FLATPAK)
        message(FATAL_ERROR "Disable the search provider, upstream tests and Flatpak on Windows")
    endif()
    if(NOT BUILD_CLI AND NOT BUILD_GUI)
        message(FATAL_ERROR "The Windows compatibility layer requires CLI or GUI")
    endif()
    find_path(OTP_WINDOWS_GETTEXT_INCLUDE libintl.h
        HINTS "${OTP_WINDOWS_GETTEXT_ROOT}/include" REQUIRED)
    find_library(OTP_WINDOWS_GETTEXT_LIBRARY intl
        HINTS "${OTP_WINDOWS_GETTEXT_ROOT}/lib" REQUIRED)
    if(BUILD_GUI)
        # The GUI header includes the common redirects as well. Use one forced
        # include group so CMake's option de-duplication cannot drop -include.
        otp_windows_apply_compat(${PROJECT_NAME} otp-windows-gui.h)
        target_sources(${PROJECT_NAME} PRIVATE "${_otp_windows_root}/gui-compat.c")
        set_source_files_properties("${_otp_windows_root}/gui-compat.c"
            TARGET_DIRECTORY ${PROJECT_NAME} PROPERTIES
            COMPILE_DEFINITIONS OTP_WINDOWS_COMPAT_IMPLEMENTATION)
        message(STATUS "Windows GUI compile shims enabled; desktop integrations remain unported")
        set(_target "${PROJECT_NAME}")
    endif()
    if(BUILD_CLI)
        otp_windows_apply_compat(${PROJECT_NAME}-cli otp-windows.h)
        set(_target "${PROJECT_NAME}-cli")
    endif()
    option(OTP_WINDOWS_SHIM_CHECKS "Build Windows-only shim regression executable" OFF)
    if(OTP_WINDOWS_SHIM_CHECKS)
        # Prefer CLI dependencies when both are built; GUI-only is also valid.
        get_target_property(_compile ${_target} COMPILE_OPTIONS)
        list(REMOVE_ITEM _compile
            "SHELL:-include \"${_otp_windows_root}/compat/otp-windows.h\""
            "SHELL:-include \"${_otp_windows_root}/compat/otp-windows-gui.h\"")
        add_executable(otp-windows-shim-check
            "${_otp_windows_root}/shim-check.c" "${_otp_windows_root}/compat.c" "${_otp_windows_root}/console.c"
            "${_otp_windows_root}/security.c" "${_otp_windows_root}/private-file.c")
        get_target_property(_includes ${_target} INCLUDE_DIRECTORIES)
        get_target_property(_libraries ${_target} LINK_LIBRARIES)
        target_include_directories(otp-windows-shim-check PRIVATE ${_includes})
        target_compile_options(otp-windows-shim-check PRIVATE ${_compile})
        target_compile_definitions(otp-windows-shim-check PRIVATE OTP_WINDOWS_COMPAT_IMPLEMENTATION)
        target_link_libraries(otp-windows-shim-check PRIVATE ${_libraries})
        add_executable(otp-windows-console-check
            "${_otp_windows_root}/console-check.c" "${_otp_windows_root}/console.c"
            "${_otp_windows_root}/compat.c" "${_otp_windows_root}/security.c"
            "${_otp_windows_root}/private-file.c")
        target_include_directories(otp-windows-console-check PRIVATE ${_includes})
        target_compile_options(otp-windows-console-check PRIVATE ${_compile})
        target_compile_definitions(otp-windows-console-check PRIVATE
            OTP_WINDOWS_COMPAT_IMPLEMENTATION OTP_WINDOWS_CONSOLE_CHECK)
        target_link_libraries(otp-windows-console-check PRIVATE ${_libraries})
    endif()
endfunction()
cmake_language(DEFER CALL otp_windows_targets)
