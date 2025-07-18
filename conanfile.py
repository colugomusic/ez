from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout
from conan.tools.build import check_min_cppstd

class EzConanFile(ConanFile):
	name = "ez"
	version = "1.0.0"
	license = "MIT"
	author = "ColugoMusic"
	url = "https://github.com/colugomusic/ez"
	description = "Realtime-safe thread synchronization"
	settings = "os", "compiler", "build_type", "arch"
	generators = "CMakeDeps", "CMakeToolchain"
	exports_sources = "CMakeLists.txt", "include/*"

	def layout(self):
		cmake_layout(self)

	def requirements(self):
		pass

	def validate(self):
		if self.settings.get_safe("compiler.cppstd"):
			check_min_cppstd(self, 20)

	def build(self):
		cmake = CMake(self)
		cmake.configure()
		cmake.build()

	def package(self):
		cmake = CMake(self)
		cmake.install()
	
	def package_info(self):
		self.cpp_info.includedirs = ["include"]