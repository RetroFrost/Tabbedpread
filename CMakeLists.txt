cmake_minimum_required(VERSION 3.24)

project(Tabbedpread VERSION 0.1.0 LANGUAGES CXX RC)

add_executable(tabbedpread WIN32
    src/main.cpp
    src/tabbedpread.rc
)

target_compile_features(tabbedpread PRIVATE cxx_std_20)
target_compile_definitions(tabbedpread PRIVATE
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
)

target_include_directories(tabbedpread PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_precompile_headers(tabbedpread PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/src/compat.h>"
)

target_link_libraries(tabbedpread PRIVATE
    dwmapi
    user32
    gdi32
    shell32
    advapi32
)

if(MSVC)
    target_compile_options(tabbedpread PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

set_target_properties(tabbedpread PROPERTIES
    OUTPUT_NAME "tabbedpread"
)

install(TARGETS tabbedpread RUNTIME DESTINATION .)
