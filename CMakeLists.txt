# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)



add_compile_options(-Wno-format)
add_compile_options(-Wno-ignored-qualifiers)
add_compile_options(-Wno-attributes)
add_compile_options(-Wno-unused-function)
project(esp_idf_project)

# ESP-IDF adds "-Werror" to every component, some third-party components
# (e.g. chmorgan/esp-audio-player) are not clean against newer IDF headers.
# Append the flag after project() so it is placed behind IDF's own options.
idf_build_get_property(build_components BUILD_COMPONENTS)
foreach(_cmp ${build_components})
    if(TARGET __idf_${_cmp})
        get_target_property(_cmp_type __idf_${_cmp} TYPE)
        if(_cmp_type STREQUAL "INTERFACE_LIBRARY")
            target_compile_options(__idf_${_cmp} INTERFACE -Wno-error=switch)
        else()
            target_compile_options(__idf_${_cmp} PRIVATE -Wno-error=switch)
        endif()
    endif()
endforeach()
