# Source retained from AlphaLoc and extended by FrameNav.
# SPDX-License-Identifier: Apache-2.0

# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(SRCS ${app_sources})
