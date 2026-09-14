#include "native/symbol/symbol_guess_windows_internal.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
// windows.h must precede psapi.h; keep clang-format from sorting this pair.
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on

#include <mutex>
#include <utility>

namespace spark::symbol_guess::windows {

TypedLabel chooseVtableLabel(std::vector<VtableEvidence> evidence)
{
    return ::spark::symbol_guess::chooseVtableLabel(std::move(evidence), nullptr);
}

int scoreStringHint(std::string_view value)
{
    return ::spark::symbol_guess::scoreStringHint(value);
}

TypedLabel formatStringHint(std::string_view value)
{
    const int score = scoreStringHint(value);
    if (score < ::spark::symbol_guess::kMinimumStringHintScore) {
        return {};
    }
    constexpr std::size_t k_maximum = 52;
    std::string message(value.substr(0, k_maximum));
    if (value.size() > k_maximum) {
        message.resize(k_maximum - 3);
        message += "...";
    }
    return ::spark::symbol_guess::formatEvidenceLabel(::spark::symbol_guess::EvidenceSource::String, message, true);
}

Engine::Engine(const std::uint8_t *image, std::size_t mapped_size, std::uint64_t load_address)
    : impl_(std::make_unique<Impl>())
{
    impl_->image = image;
    impl_->mapped_size = mapped_size;
    impl_->load_address = load_address;
    impl_->initialize();
}

Engine::~Engine() = default;
Engine::Engine(Engine &&) noexcept = default;
Engine &Engine::operator=(Engine &&) noexcept = default;

bool Engine::valid() const
{
    return impl_ != nullptr && impl_->stats.initialized;
}

const FunctionRange *Engine::functionContaining(std::uint64_t rva) const
{
    return impl_ != nullptr ? impl_->containing(rva) : nullptr;
}

std::unordered_map<std::uint64_t, TypedLabel> Engine::guess(std::span<const std::uint64_t> rvas)
{
    return impl_ != nullptr ? impl_->guess(rvas) : std::unordered_map<std::uint64_t, TypedLabel>{};
}

BuildStats Engine::stats() const
{
    if (impl_ == nullptr) {
        return {};
    }
    std::scoped_lock lock(impl_->mutex);
    return impl_->stats;
}

namespace {

struct CurrentEngineState {
    std::mutex mutex;
    std::unique_ptr<Engine> engine;
};

CurrentEngineState &currentEngineState()
{
    static CurrentEngineState state;
    return state;
}

Engine &currentEngine()
{
    CurrentEngineState &state = currentEngineState();
    std::scoped_lock lock(state.mutex);
    if (state.engine != nullptr) {
        return *state.engine;
    }
    state.engine = std::make_unique<Engine>([] {
        HMODULE module = GetModuleHandleW(nullptr);
        MODULEINFO info{};
        if (module == nullptr || GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)) == FALSE) {
            return Engine(nullptr, 0, 0);
        }
        return Engine(static_cast<const std::uint8_t *>(info.lpBaseOfDll), info.SizeOfImage,
                      reinterpret_cast<std::uint64_t>(info.lpBaseOfDll));
    }());
    return *state.engine;
}

}  // namespace

std::unordered_map<std::uint64_t, TypedLabel> guessCurrentModuleSymbols(std::span<const std::uint64_t> rvas)
{
    if (rvas.empty()) {
        return {};
    }
    return currentEngine().guess(rvas);
}

BuildStats currentModuleStats()
{
    CurrentEngineState &state = currentEngineState();
    std::scoped_lock lock(state.mutex);
    return state.engine != nullptr ? state.engine->stats() : BuildStats{};
}

}  // namespace spark::symbol_guess::windows

#endif
