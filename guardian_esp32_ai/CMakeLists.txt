cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.0.0")

add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(guardian_esp32_ai)
