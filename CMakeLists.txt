cmake_minimum_required(VERSION 3.20)

project(KeepAwake LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# CRT вшивается в exe: программе не нужны никакие библиотеки на компьютере.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(KeepAwake WIN32
    src/main.cpp
    src/KeepAwake.rc
)

# Пересобирать ресурсы, когда меняются иконки или манифест.
set_source_files_properties(src/KeepAwake.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/icons/KeepAwake-On.ico;${CMAKE_CURRENT_SOURCE_DIR}/src/icons/KeepAwake-Off.ico;${CMAKE_CURRENT_SOURCE_DIR}/src/KeepAwake.manifest")

target_compile_definitions(KeepAwake PRIVATE UNICODE _UNICODE)
target_link_libraries(KeepAwake PRIVATE comctl32 shell32 advapi32 user32)

if(MSVC)
    # Исходники в UTF-8; манифест уже вшит через KeepAwake.rc.
    target_compile_options(KeepAwake PRIVATE /utf-8 /W4 /permissive- $<$<CONFIG:Release>:/O1>)
    target_link_options(KeepAwake PRIVATE /MANIFEST:NO $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)
else()
    # MinGW: статическая сборка без зависимостей от DLL MinGW.
    target_compile_options(KeepAwake PRIVATE
        -Wall -Wextra -municode -fno-exceptions -fno-rtti
        -ffunction-sections -fdata-sections
        $<$<CONFIG:Release>:-Os>)
    target_link_options(KeepAwake PRIVATE
        -municode -static -Wl,--gc-sections
        $<$<CONFIG:Release>:-s>)
endif()
