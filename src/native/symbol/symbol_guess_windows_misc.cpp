#include "native/symbol/symbol_guess_windows_internal.h"

#ifdef _WIN32

#include <chrono>
#include <map>

namespace spark::symbol_guess::windows {

void Engine::Impl::updateApproximateBytes()
{
    std::size_t bytes = ranges.capacity() * sizeof(FunctionRange) + sections.capacity() * sizeof(Section);
    for (const auto &[root, fragments] : chained_fragment_starts) {
        bytes += sizeof(root) + fragments.capacity() * sizeof(std::uint32_t) + 32;
    }
    for (const auto &[root, label] : vtable_labels) {
        bytes += sizeof(root) + label.label.capacity() + 48;
    }
    stats.approximate_bytes = bytes;
}

void Engine::Impl::initialize()
{
    const auto start = std::chrono::steady_clock::now();
    if (!parseHeaders()) {
        return;
    }
    collectFunctions();
    if (ranges.empty()) {
        return;
    }
    collectVtables();
    updateApproximateBytes();
    stats.initialized = true;
    stats.build_microseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
}

std::unordered_map<std::uint64_t, TypedLabel> Engine::Impl::guess(std::span<const std::uint64_t> rvas)
{
    std::scoped_lock lock(mutex);
    const auto start = std::chrono::steady_clock::now();
    BuildStats batch = stats;
    batch.batch_microseconds = 0;
    batch.sampled_functions = 0;
    batch.decoded_instructions = 0;
    batch.string_candidates = 0;
    batch.shared_strings = 0;
    batch.string_labels = 0;
    batch.string_reference_candidates = 0;
    batch.string_reference_potential_hits = 0;
    batch.string_reference_exact_hits = 0;
    batch.string_reference_interior_rejections = 0;
    batch.string_reference_ambiguities = 0;
    batch.string_reference_shared = 0;
    batch.string_reference_terminal_hits_skipped = 0;
    batch.string_reference_unindexed = 0;
    batch.string_reference_unreachable = 0;
    batch.string_reference_overlaps = 0;
    batch.string_validation_functions = 0;
    batch.string_function_byte_budget_exhausted = 0;
    batch.string_function_instruction_budget_exhausted = 0;
    batch.string_validation_budget_exhausted = 0;
    batch.string_instruction_budget_exhausted = 0;
    batch.string_scan_byte_budget_exhausted = 0;

    std::unordered_map<std::uint64_t, TypedLabel> out;
    out.reserve(rvas.size());
    std::map<std::uint32_t, std::vector<std::uint64_t>> root_inputs;
    for (std::uint64_t rva : rvas) {
        if (const FunctionRange *function = containing(rva)) {
            root_inputs[function->root].push_back(rva);
        }
    }
    batch.sampled_functions = root_inputs.size();

    std::map<std::uint32_t, std::vector<StringCandidate>> string_candidates;
    std::unordered_set<std::uint32_t> candidate_targets;
    std::map<std::uint32_t, RootValidation> validations;
    ValidationBudget validation_budget;
    for (const auto &[root, inputs] : root_inputs) {
        if (const auto label = vtable_labels.find(root); label != vtable_labels.end()) {
            for (std::uint64_t rva : inputs) {
                out.emplace(rva, label->second);
            }
            continue;
        }
        std::vector<StringCandidate> candidates = decodeStrings(root, batch, validations, validation_budget);
        for (const StringCandidate &candidate : candidates) {
            candidate_targets.insert(candidate.target);
        }
        string_candidates.emplace(root, std::move(candidates));
    }

    std::map<std::uint32_t, std::set<std::uint32_t>> references;
    for (const auto &[root, candidates] : string_candidates) {
        for (const StringCandidate &candidate : candidates) {
            if (references[candidate.target].insert(root).second) {
                if (batch.string_reference_exact_hits < 1000000U) {
                    ++batch.string_reference_exact_hits;
                }
            }
        }
    }
    if (!candidate_targets.empty()) {
        scanCandidateReferences(candidate_targets, references, validations, validation_budget, batch);
    }
    for (const auto &[root, candidates] : string_candidates) {
        TypedLabel label;
        for (const StringCandidate &candidate : candidates) {
            const auto refs = references.find(candidate.target);
            if (refs != references.end() && refs->second.size() == 1 && *refs->second.begin() == root) {
                label = ::spark::symbol_guess::windows::formatStringHint(candidate.value);
                break;
            }
            ++batch.shared_strings;
        }
        if (label.empty()) {
            continue;
        }
        ++batch.string_labels;
        for (std::uint64_t rva : root_inputs.at(root)) {
            out.emplace(rva, label);
        }
    }
    batch.batch_microseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    stats = batch;
    return out;
}

}  // namespace spark::symbol_guess::windows

#endif
