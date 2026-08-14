#include "native/symbol/symbolicate.h"

#if defined(__linux__) && defined(__x86_64__)

#include <dlfcn.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <cpptrace/cpptrace.hpp>

namespace {

int failures = 0;

#define CHECK(expr)                                                                       \
    do {                                                                                  \
        if (!(expr)) {                                                                    \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #expr << '\n'; \
            ++failures;                                                                   \
        }                                                                                 \
    } while (false)

}  // namespace

int main()
{
    void *fixture = ::dlopen(SPARK_HIDDEN_SYMBOL_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) {
        return 1;
    }

    using AddressFunction = std::uintptr_t (*)();
    auto address_function = reinterpret_cast<AddressFunction>(::dlsym(fixture, "sparkHiddenSymbolFixtureAddress"));
    CHECK(address_function != nullptr);
    if (address_function == nullptr) {
        ::dlclose(fixture);
        return 1;
    }

    const std::uintptr_t address = address_function();
    Dl_info dynamic_info{};
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    CHECK(::dladdr(reinterpret_cast<void *>(address), &dynamic_info) != 0);
    CHECK(dynamic_info.dli_sname == nullptr);

    cpptrace::safe_object_frame object{};
    cpptrace::get_safe_object_frame(address, &object);
    CHECK(object.object_path[0] != '\0');
    CHECK(object.address_relative_to_object_start != 0);

    spark::ModuleTable modules;
    const spark::ModuleId fixture_module = modules.intern(object.object_path);
    const spark::ModuleId missing_module = modules.intern("/missing/spark-hidden-symbol-fixture.so");
    const spark::FrameKey hidden{
        .module = fixture_module, .rva = object.address_relative_to_object_start, .raw_address = address};
    const spark::FrameKey missing{.module = missing_module, .rva = 0x42, .raw_address = 1};

    const auto resolved = spark::resolveFrames(modules, std::vector<spark::FrameKey>{hidden, missing});
    const auto hidden_result = resolved.find(hidden);
    CHECK(hidden_result != resolved.end());
    if (hidden_result != resolved.end()) {
        CHECK(hidden_result->second.class_name.find("spark_hidden_symbol_fixture") != std::string::npos);
        CHECK(hidden_result->second.method_name.find("hiddenProfileTarget") != std::string::npos);
    }

    const auto missing_result = resolved.find(missing);
    CHECK(missing_result != resolved.end());
    if (missing_result != resolved.end()) {
        CHECK(missing_result->second.method_name == "0x42");
    }

    ::dlclose(fixture);
    if (failures != 0) {
        std::cerr << failures << " hidden symbol test(s) failed\n";
        return 1;
    }
    std::cout << "Hidden symbol tests passed\n";
    return 0;
}

#endif
