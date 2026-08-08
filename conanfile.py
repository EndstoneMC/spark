from conan import ConanFile
from conan.tools.cmake import cmake_layout


class SparkRecipe(ConanFile):
    name = "spark"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeToolchain", "CMakeDeps"

    def layout(self):
        cmake_layout(self)

    def requirements(self):
        self.requires("cpptrace/1.0.4")
        self.requires("concurrentqueue/1.0.4")
        self.requires("zlib/1.3.1")
        self.requires("expected-lite/0.9.0")
        self.requires("libcurl/8.21.0")
        if self.settings.os != "Windows":
            self.requires("openssl/3.6.3")
