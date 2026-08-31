cmake_minimum_required(VERSION 3.20)

project(KeyPulse VERSION 0.0.2 LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "KeyPulse is a Windows desktop application.")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "KeyPulse currently requires an MSVC-compatible toolchain.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(KEYPULSE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" CACHE PATH
    "Directory used for the KeyPulse executable")

function(keypulse_configure_target target)
    target_compile_definitions(${target} PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        WINVER=0x0601
        _WIN32_WINNT=0x0601
    )
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
    set_property(TARGET ${target} PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endfunction()

add_executable(KeyPulse WIN32
    src/main.cpp
    src/input_merge.cpp
    src/input_merge.h
    src/startup.cpp
    src/startup.h
    src/window_layout.cpp
    src/window_layout.h
    src/storage.cpp
    src/storage.h
    KeyPulse.rc
    resource.h
    icon/keypulse.ico
    icon/key-press-100.png
    icon/key-press-50.png
)
keypulse_configure_target(KeyPulse)
target_link_options(KeyPulse PRIVATE /MANIFEST:NO)
set_source_files_properties(icon/keypulse.ico icon/key-press-100.png icon/key-press-50.png
    PROPERTIES HEADER_FILE_ONLY TRUE)
target_link_libraries(KeyPulse PRIVATE
    d2d1
    dwrite
    windowscodecs
    advapi32
    shell32
    ole32
    user32
    gdi32
    comctl32
)

set_target_properties(KeyPulse PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${KEYPULSE_RUNTIME_OUTPUT_DIRECTORY}")
foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" config_upper)
    set_target_properties(KeyPulse PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${KEYPULSE_RUNTIME_OUTPUT_DIRECTORY}")
endforeach()

include(CTest)
if(BUILD_TESTING)
    add_executable(KeyPulseStorageTests
        src/storage_tests.cpp
        src/storage.cpp
        src/storage.h
    )
    keypulse_configure_target(KeyPulseStorageTests)
    target_link_libraries(KeyPulseStorageTests PRIVATE shell32 ole32)

    set_target_properties(KeyPulseStorageTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    foreach(config Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${config}" config_upper)
        set_target_properties(KeyPulseStorageTests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/tests")
    endforeach()

    add_test(NAME storage_round_trip
        COMMAND KeyPulseStorageTests "${CMAKE_BINARY_DIR}/storage-test.kpf")

    add_executable(KeyPulseInputMergeTests
        src/input_merge_tests.cpp
        src/input_merge.cpp
        src/input_merge.h
    )
    keypulse_configure_target(KeyPulseInputMergeTests)
    set_target_properties(KeyPulseInputMergeTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    foreach(config Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${config}" config_upper)
        set_target_properties(KeyPulseInputMergeTests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/tests")
    endforeach()
    add_test(NAME input_merge
        COMMAND KeyPulseInputMergeTests)

    add_executable(KeyPulseWindowLayoutTests
        src/window_layout_tests.cpp
        src/window_layout.cpp
        src/window_layout.h
    )
    keypulse_configure_target(KeyPulseWindowLayoutTests)
    set_target_properties(KeyPulseWindowLayoutTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    foreach(config Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${config}" config_upper)
        set_target_properties(KeyPulseWindowLayoutTests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/tests")
    endforeach()
    add_test(NAME window_layout
        COMMAND KeyPulseWindowLayoutTests)
endif()

install(TARGETS KeyPulse RUNTIME DESTINATION .)
set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT KeyPulse)
