#include "platform/endstone/native_plugin_attribution.h"

#include <cstring>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

#include <endstone/plugin/plugin.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace spark::endstone_adapter {
namespace {

struct AddressModule {
    std::uintptr_t base = 0;
    std::uintptr_t query_base = 0;
    std::string path;
    bool native_entrypoint = false;
};

#ifdef _WIN32
std::string modulePath(HMODULE module)
{
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            return std::filesystem::path(std::wstring(buffer.data(), length)).string();
        }
        if (buffer.size() >= 32768) {
            return {};
        }
        buffer.resize(buffer.size() * 2);
    }
}

AddressModule resolveAddress(const void *address)
{
    AddressModule result;
    if (address == nullptr) {
        return result;
    }

    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) != FALSE) {
        result.base = reinterpret_cast<std::uintptr_t>(module);
        result.path = modulePath(module);
        result.native_entrypoint = GetProcAddress(module, "init_endstone_plugin") != nullptr;
    }

    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) == sizeof(memory)) {
        result.query_base = reinterpret_cast<std::uintptr_t>(memory.AllocationBase);
    }
    return result;
}
#else
AddressModule resolveAddress(const void *address)
{
    AddressModule result;
    if (address == nullptr) {
        return result;
    }

    Dl_info info{};
    if (dladdr(address, &info) == 0) {
        return result;
    }
    result.base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    result.query_base = result.base;
    if (info.dli_fname != nullptr) {
        result.path = info.dli_fname;
#ifdef RTLD_NOLOAD
        void *module = dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD);
        if (module != nullptr) {
            result.native_entrypoint = dlsym(module, "init_endstone_plugin") != nullptr;
            dlclose(module);
        }
#endif
    }
    return result;
}
#endif

}  // namespace

std::optional<NativePluginModuleIdentity> identifyNativePluginModule(const ::endstone::Plugin &plugin)
{
    const void *vtable = nullptr;
    std::memcpy(&vtable, static_cast<const void *>(std::addressof(plugin)), sizeof(vtable));

    const void *first_virtual = nullptr;
    if (vtable != nullptr) {
        std::memcpy(&first_virtual, vtable, sizeof(first_virtual));
    }

    const AddressModule vtable_module = resolveAddress(vtable);
    const AddressModule first_virtual_module = resolveAddress(first_virtual);
    if (!vtable_module.native_entrypoint || vtable_module.base == 0 || vtable_module.base != vtable_module.query_base ||
        vtable_module.base != first_virtual_module.base) {
        return std::nullopt;
    }
    return NativePluginModuleIdentity{.module_base = vtable_module.base, .module_path = std::move(vtable_module.path)};
}

}  // namespace spark::endstone_adapter
