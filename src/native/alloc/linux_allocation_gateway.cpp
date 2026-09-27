#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include "native/alloc/linux_allocation_gateway_abi.h"
#include "native/alloc/linux_elf_admission.h"
#include "native/alloc/linux_permanent_gateway_registry.h"
#include "spark_gateway_compatibility.h"

namespace {

using spark::gateway::permanent::Group;
using spark::gateway::permanent::KCapacity;
using spark::gateway::permanent::KClosed;
using spark::gateway::permanent::KCodeStride;
using spark::gateway::permanent::KProviderCapacity;
using spark::gateway::permanent::Lock;
using spark::gateway::permanent::Provider;
using spark::gateway::permanent::State;

State *storage() noexcept
{
    return spark::gateway::permanent::state();
}

int failure(char *error, std::size_t size, const char *message)
{
    if (error != nullptr && size != 0) {
        std::snprintf(error, size, "%s", message);
    }
    return 0;
}

int bootstrap(const char *root, const void *anchor, char *error, std::size_t size)
{
    std::string detail;
    return spark::gateway::permanent::bootstrap(root, anchor, detail) ? 1 : failure(error, size, detail.c_str());
}

const char *installation()
{
    const auto *directory = spark::gateway::permanent::directory();
    return directory != nullptr ? directory->installation : "";
}

SparkGatewayBindingV1 binding(std::size_t index)
{
    SparkGatewayBindingV1 result{sizeof(SparkGatewayBindingV1), static_cast<std::uint32_t>(index), {}, nullptr};
    const auto start = spark::gateway::permanent::directory()->code + index * 8 * KCodeStride;
    for (std::size_t api = 0; api < 7; ++api) {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        result.entries[api] = reinterpret_cast<void *>(start + api * KCodeStride);
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    result.tls_entry = reinterpret_cast<void (*)(void *)>(start + 7 * KCodeStride);
    return result;
}

int reserve(void *const *originals, const void *spark_address, SparkGatewayBindingV1 *result, char *error,
            std::size_t size)
{
    Lock lock(storage());
    if (storage() == nullptr || originals == nullptr || result == nullptr || result->size != sizeof(*result)) {
        return failure(error, size, "allocation gateway is not initialized");
    }
    for (const auto &group : storage()->groups) {
        if (group.reserved && !group.published && !group.retired) {
            return failure(error, size, "another allocation gateway binding awaits publication");
        }
    }
    std::size_t index = 0;
    while (index < KCapacity && storage()->groups[index].reserved) {
        ++index;
    }
    if (index == KCapacity) {
        return failure(error, size, "allocation gateway lifetime capacity exhausted; restart required");
    }
    std::array<Provider, 7> acquired{};
    std::array<spark::gateway::LoaderHandle, 7> handles;
    std::size_t acquired_count = 0;
    try {
        spark::gateway::elf::Admission admission(spark_address);
        for (std::size_t api = 0; api < 7; ++api) {
            const auto index = admission.snapshot.owner(originals[api], PF_R | PF_X);
            const auto &provider = admission.snapshot.objects[index].identity;
            if ((!admission.resident.contains(index) && !admission.preloadedAllocator(index)) ||
                index == admission.spark || spark::gateway::elf::sparkObject(admission.snapshot.objects[index])) {
                return failure(error, size, "allocator original has an unsafe provider");
            }
            spark::gateway::LoaderHandle handle(spark::gateway::lease(provider));
            spark::gateway::loaderFault(10 + api);
            if (!handle) {
                return failure(error, size, "cannot retain allocator provider");
            }
            const auto provider_name = std::filesystem::path(provider.path).filename().string();
            if (!admission.resident.contains(index) && provider_name.starts_with("libjemalloc.so")) {
                using Mallctl = int (*)(const char *, void *, std::size_t *, void *, std::size_t);
                auto *mallctl = reinterpret_cast<Mallctl>(::dlsym(handle.get(), "mallctl"));
                const char *zero_realloc = nullptr;
                std::size_t length = sizeof(zero_realloc);
                if (mallctl == nullptr ||
                    admission.snapshot.owner(reinterpret_cast<const void *>(mallctl), PF_R | PF_X) != index ||
                    mallctl("opt.zero_realloc", &zero_realloc, &length, nullptr, 0) != 0 ||
                    length != sizeof(zero_realloc) || zero_realloc == nullptr ||
                    std::strcmp(zero_realloc, "free") != 0) {
                    return failure(error, size, "unsupported jemalloc zero_realloc configuration");
                }
            }
            else if (!admission.resident.contains(index) && provider_name.starts_with("libmimalloc.so")) {
                using MiVersion = int (*)();
                auto *version = reinterpret_cast<MiVersion>(::dlsym(handle.get(), "mi_version"));
                if (version == nullptr ||
                    admission.snapshot.owner(reinterpret_cast<const void *>(version), PF_R | PF_X) != index ||
                    version() <= 0) {
                    return failure(error, size, "unsupported mimalloc provider");
                }
            }
            const auto same = [&provider](const Provider &entry) {
                return entry.base == provider.base && entry.device == provider.device && entry.inode == provider.inode;
            };
            if (spark::gateway::mainExecutable(provider) ||
                std::find_if(storage()->providers.begin(), storage()->providers.begin() + storage()->provider_count,
                             same) != storage()->providers.begin() + storage()->provider_count ||
                std::find_if(acquired.begin(), acquired.begin() + acquired_count, same) !=
                    acquired.begin() + acquired_count) {
            }
            else {
                spark::gateway::loaderFault(20 + api);
                acquired[acquired_count++] = {
                    .base = provider.base, .device = provider.device, .inode = provider.inode, .handle = handle.get()};
                handles[acquired_count - 1] = std::move(handle);
            }
        }
        if (storage()->provider_count + acquired_count > KProviderCapacity) {
            return failure(error, size, "allocation gateway provider capacity exhausted");
        }
        Group &group = storage()->groups[index];
        spark::gateway::elf::require(admission.snapshot.unchanged(), "loader changed during provider admission");
        for (std::size_t api = 0; api < 7; ++api) {
            group.entries[api].original = originals[api];
        }
        for (std::size_t i = 0; i < acquired_count; ++i) {
            storage()->providers[storage()->provider_count++] = acquired[i];
            group.pending_leases[group.pending_count++] = acquired[i].handle;
            handles[i].release();
        }
        group.reserved = true;
        *result = binding(index);
        return 1;
    }
    catch (const std::exception &exception) {
        return failure(error, size, exception.what());
    }
    catch (...) {
        return failure(error, size, "cannot reserve allocation gateway");
    }
}

int open(std::uint32_t index, const SparkGatewayCallbacksV1 *callbacks, void *context, int tls)
{
    Lock lock(storage());
    if (index >= KCapacity || callbacks == nullptr || context == nullptr || !storage()->groups[index].reserved ||
        storage()->groups[index].retired || storage()->groups[index].final_close) {
        return 0;
    }
    std::array<void *, 8> values{reinterpret_cast<void *>(callbacks->malloc_callback),
                                 reinterpret_cast<void *>(callbacks->calloc_callback),
                                 reinterpret_cast<void *>(callbacks->realloc_callback),
                                 reinterpret_cast<void *>(callbacks->free_callback),
                                 reinterpret_cast<void *>(callbacks->reallocarray_callback),
                                 reinterpret_cast<void *>(callbacks->aligned_alloc_callback),
                                 reinterpret_cast<void *>(callbacks->posix_memalign_callback),
                                 reinterpret_cast<void *>(callbacks->tls_callback)};
    const std::size_t begin = tls != 0 ? 7 : 0;
    const std::size_t end = tls != 0 ? 8 : 7;
    for (std::size_t api = begin; api < end; ++api) {
        const auto state = storage()->groups[index].entries[api].state.load(std::memory_order_acquire);
        const auto &entry = storage()->groups[index].entries[api];
        if (values[api] == nullptr || state != KClosed || entry.callback != nullptr || entry.context != nullptr) {
            return 0;
        }
    }
    for (std::size_t api = begin; api < end; ++api) {
        auto &entry = storage()->groups[index].entries[api];
        entry.callback = values[api];
        entry.context = context;
        entry.state.store(0, std::memory_order_release);
    }
    return 1;
}

void close(std::uint32_t index, int final)
{
    if (index >= KCapacity) {
        return;
    }
    Lock lock(storage());
    if (final != 0) {
        storage()->groups[index].final_close = true;
    }
    const std::size_t end = final != 0 ? 8 : 7;
    for (std::size_t api = 0; api < end; ++api) {
        storage()->groups[index].entries[api].state.fetch_or(KClosed, std::memory_order_acq_rel);
    }
}

std::uint64_t active(std::uint32_t index, int tls)
{
    if (index >= KCapacity) {
        return 0;
    }
    std::uint64_t total = 0;
    const std::size_t begin = tls != 0 ? 7 : 0;
    const std::size_t end = tls != 0 ? 8 : 7;
    for (std::size_t api = begin; api < end; ++api) {
        total += storage()->groups[index].entries[api].state.load(std::memory_order_acquire) & ~KClosed;
    }
    return total;
}

int clear(std::uint32_t index, int tls)
{
    Lock lock(storage());
    if (index >= KCapacity) {
        return 0;
    }
    const std::size_t begin = tls != 0 ? 7 : 0;
    const std::size_t end = tls != 0 ? 8 : 7;
    for (std::size_t api = begin; api < end; ++api) {
        if (storage()->groups[index].entries[api].state.load(std::memory_order_acquire) != KClosed) {
            return 0;
        }
    }
    for (std::size_t api = begin; api < end; ++api) {
        storage()->groups[index].entries[api].callback = nullptr;
        storage()->groups[index].entries[api].context = nullptr;
    }
    return 1;
}

void publish(std::uint32_t index)
{
    Lock lock(storage());
    if (index < KCapacity && storage()->groups[index].reserved) {
        storage()->groups[index].published = true;
        storage()->groups[index].pending_count = 0;
    }
}

int retire(std::uint32_t index)
{
    if (!clear(index, 0) || !clear(index, 1)) {
        return 0;
    }
    Lock lock(storage());
    if (!storage()->groups[index].final_close) {
        return 0;
    }
    storage()->groups[index].retired = true;
    return 1;
}

int cancel(std::uint32_t index)
{
    Lock lock(storage());
    if (index >= KCapacity || !storage()->groups[index].reserved || storage()->groups[index].published) {
        return 0;
    }
    auto &group = storage()->groups[index];
    for (auto &entry : group.entries) {
        if (entry.state.load(std::memory_order_acquire) != KClosed) {
            return 0;
        }
    }
    // Unpublished reservations are serialized until publish or cancellation.
    for (std::size_t i = 0; i < group.pending_count; ++i) {
        for (std::size_t provider = 0; provider < storage()->provider_count; ++provider) {
            if (storage()->providers[provider].handle == group.pending_leases[i]) {
                ::dlclose(storage()->providers[provider].handle);
                storage()->providers[provider] = storage()->providers[--storage()->provider_count];
                break;
            }
        }
    }
    group.pending_count = 0;
    group.reserved = false;
    group.final_close = false;
    group.retired = false;
    for (auto &entry : group.entries) {
        entry.original = nullptr;
        entry.callback = nullptr;
        entry.context = nullptr;
    }
    return 1;
}

std::uint32_t used()
{
    Lock lock(storage());
    std::uint32_t count = 0;
    for (const auto &group : storage()->groups) {
        count += group.reserved ? 1 : 0;
    }
    return count;
}

std::uint32_t leases()
{
    Lock lock(storage());
    return storage()->provider_count;
}

// NOLINTNEXTLINE(readability-non-const-parameter)
void setTestGate(std::uint32_t index, std::uint32_t api, std::uint32_t phase, std::uint64_t *gate)
{
#if defined(SPARK_GATEWAY_TESTING)
    if (index < KCapacity && api < 8) {
        storage()->groups[index].entries[api].gate = gate;
        storage()->groups[index].entries[api].phase = phase;
    }
#else
    (void)index;
    (void)api;
    (void)phase;
    (void)gate;
#endif
}

const SparkGatewayV1 Api{sizeof(SparkGatewayV1),
                         SPARK_GATEWAY_ABI_VERSION,
                         SPARK_GATEWAY_FAMILY,
                         SPARK_GATEWAY_COMPATIBILITY,
                         KCapacity,
                         KProviderCapacity,
                         &bootstrap,
                         &installation,
                         &reserve,
                         &open,
                         &close,
                         &active,
                         &clear,
                         &retire,
                         &publish,
                         &cancel,
                         &used,
                         &leases,
#if defined(SPARK_GATEWAY_TESTING)
                         &setTestGate
#else
                         nullptr
#endif
};

}  // namespace

extern "C" const SparkGatewayV1 *spark_allocation_gateway_v1()
{
    return &Api;
}
