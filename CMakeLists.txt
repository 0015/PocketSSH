# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Project version
set(PROJECT_VER "1.2.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(PocketSSH)
