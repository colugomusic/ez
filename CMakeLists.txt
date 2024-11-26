cmake_minimum_required(VERSION 3.20)
project(ez)
include(CPM.cmake)
CPMAddPackage(NAME cs_libguarded GITHUB_REPOSITORY copperspice/cs_libguarded GIT_TAG libguarded-1.4.1 DOWNLOAD_ONLY YES)
add_library(ez INTERFACE include/ez.hpp)
target_include_directories(ez PUBLIC
	${CMAKE_CURRENT_LIST_DIR}/include
	${cs_libguarded_SOURCE_DIR}/src
)
target_compile_options(ez PRIVATE
	$<$<CXX_COMPILER_ID:MSVC>:/W3 /WX>
)
add_library(ez::ez ALIAS ez)
