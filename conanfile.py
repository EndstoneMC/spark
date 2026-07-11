from conan import ConanFile
from conan.tools.cmake import cmake_layout


class EndstoneSparkRecipe(ConanFile):
    settings = "os", "arch", "compiler", "build_type"
    generators = "CMakeToolchain", "CMakeDeps"
    options = {"fPIC": [True, False]}
    default_options = {
        "fPIC": True,
        "cpptrace/*:unwind": "libunwind",
    }

    def requirements(self):
        self.requires("cpptrace/1.0.4")
        self.requires("concurrentqueue/1.0.4")
        self.requires("zlib/1.3.1") 
        self.requires("expected-lite/0.9.0")

    def layout(self):
        cmake_layout(self)
