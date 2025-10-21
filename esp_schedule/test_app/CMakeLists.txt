cmake_minimum_required(VERSION 3.16)

# Make the esp_schedule component available to this test app
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/..)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(test_app)
