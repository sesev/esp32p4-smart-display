cmake_minimum_required(VERSION 3.16)

# common_components lives alongside the 1-Demo examples; path is relative to
# this project root so it works regardless of where the repo is cloned.
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/components
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

add_compile_options(-Wno-format -Wno-ignored-qualifiers -Wno-unused-function)

project(concept)

