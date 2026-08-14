#include <cstdint>

namespace spark_fixture {
namespace {

__attribute__((visibility("hidden"), noinline)) int hiddenProfileTarget(int value)
{
    return value + 17;
}

}  // namespace
}  // namespace spark_fixture

extern "C" __attribute__((visibility("default"))) std::uintptr_t sparkHiddenSymbolFixtureAddress()
{
    return reinterpret_cast<std::uintptr_t>(&spark_fixture::hiddenProfileTarget);
}
