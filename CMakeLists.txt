cmake_minimum_required(VERSION 3.20)
project(ez)
add_library(ez INTERFACE include/ez.hpp)
add_library(ez::ez ALIAS ez)
target_sources(ez INTERFACE
	FILE_SET HEADERS
	BASE_DIRS include
	FILES
		include/ez.hpp
		include/ez-beach.hpp
		include/ez-tags.hpp
		include/ez-trigger.hpp
)
install(TARGETS ez
	FILE_SET HEADERS
)
