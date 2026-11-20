# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# 工厂测试放在独立文件夹中，以标准 ESP-IDF 组件方式加入本工程
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/factorytest")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ESP_Watch)
