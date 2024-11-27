cmake_minimum_required(VERSION 3.20)
project(ez)
include(CPM.cmake)
add_library(ez INTERFACE include/ez.hpp)
target_include_directories(ez INTERFACE
	${CMAKE_CURRENT_LIST_DIR}/include
)
add_library(ez::ez ALIAS ez)
