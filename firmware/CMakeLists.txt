cmake_minimum_required(VERSION 3.16)

# common_components lives alongside the 1-Demo examples; path is relative to
# this project root so it works regardless of where the repo is cloned.
set(EXTRA_COMPONENT_DIRS
    ../1-Demo/idf-examples/common_components
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

add_compile_options(-Wno-format -Wno-ignored-qualifiers -Wno-unused-function)

project(concept)

