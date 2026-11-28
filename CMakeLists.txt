cmake_minimum_required(VERSION 3.18)
project(SplatPCCharacterCustomizer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(character_customizer_core STATIC
    src/appearance_registry.cpp
    src/bfres_customizer.cpp
    src/customizer_hooks.cpp
    src/ui_assets.cpp
    src/customizer_ui.cpp
)

target_include_directories(character_customizer_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(WIN32)
    if(MSVC)
        target_compile_options(character_customizer_core PUBLIC /utf-8 /EHsc /W4)
        target_compile_definitions(character_customizer_core PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    else()
        target_compile_options(character_customizer_core PUBLIC -Wall -Wextra)
    endif()
else()
    target_compile_options(character_customizer_core PRIVATE -fPIC -Wall -Wextra)
    find_package(Threads REQUIRED)
    target_link_libraries(character_customizer_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
    find_library(MATH_LIB m)
    if(MATH_LIB)
        target_link_libraries(character_customizer_core PUBLIC ${MATH_LIB})
    endif()
endif()

add_library(character_customizer MODULE src/mod_entry.cpp)
target_link_libraries(character_customizer PRIVATE character_customizer_core)
set_target_properties(character_customizer PROPERTIES PREFIX "")
if(MINGW)
    target_link_options(character_customizer PRIVATE -static)
elseif(MSVC)
    target_compile_options(character_customizer PRIVATE /utf-8 /W4)
endif()

option(CUSTOMIZER_BUILD_TESTS "Build Character Customizer unit and integration tests" ON)
if(CUSTOMIZER_BUILD_TESTS)
    enable_testing()

    add_executable(character_customizer_tests tests/test_character_customizer.cpp)
    target_link_libraries(character_customizer_tests PRIVATE character_customizer_core)
    if(MSVC)
        target_compile_options(character_customizer_tests PRIVATE /utf-8 /W4)
    endif()
    add_test(NAME character_customizer_tests COMMAND character_customizer_tests)
endif()
