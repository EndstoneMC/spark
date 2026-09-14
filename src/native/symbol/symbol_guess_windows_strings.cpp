#include "native/symbol/symbol_guess_windows_internal.h"

#ifdef _WIN32

#include <distorm.h>
#include <mnemonics.h>

#include <algorithm>
#include <limits>
#include <map>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace spark::symbol_guess::windows {

namespace {

constexpr std::size_t KMaximumDiagnosticCounter = 1000000U;

void incrementDiagnostic(std::size_t &counter)
{
    if (counter < KMaximumDiagnosticCounter) {
        ++counter;
    }
}

bool isEligibleStringLea(const _DInst &instruction)
{
    return instruction.opcode == I_LEA && instruction.opsNo == 2 && instruction.ops[0].type == O_REG &&
           instruction.ops[0].size == 64 && instruction.ops[1].type == O_SMEM && instruction.ops[1].index == R_RIP &&
           (instruction.flags & FLAG_RIP_RELATIVE) != 0 &&
           (instruction.flags & (FLAG_LOCK | FLAG_REPNZ | FLAG_REP)) == 0 &&
           FLAG_GET_ADDRSIZE(instruction.flags) == 2 &&
           (instruction.segment == R_NONE || SEGMENT_IS_DEFAULT(instruction.segment));
}

std::optional<_DInst> decodeOne(const std::uint8_t *code, std::uint32_t rva, std::size_t size)
{
    const auto maximum_int = std::numeric_limits<int>::max();
    if (code == nullptr || size == 0 || size > static_cast<std::size_t>(maximum_int)) {
        return std::nullopt;
    }
    _CodeInfo info{};
    info.codeOffset = rva;
    info.code = code;
    info.codeLen = static_cast<int>(size);
    info.dt = Decode64Bits;
    info.features = DF_STOP_ON_UNDECODEABLE;
    _DInst instruction{};
    unsigned used = 0;
    const _DecodeResult result = distorm_decompose64(&info, &instruction, 1, &used);
    if ((result == DECRES_INPUTERR || result == DECRES_NONE) || used != 1 || instruction.flags == FLAG_NOT_DECODABLE ||
        instruction.size == 0 || instruction.addr != rva || instruction.size > size) {
        return std::nullopt;
    }
    return instruction;
}

bool addRipDisplacement(std::uint32_t opcode_rva, std::int32_t displacement, std::uint32_t &target)
{
    const std::int64_t value = static_cast<std::int64_t>(opcode_rva) + 6 + displacement;
    if (value < 0 || std::cmp_greater(value, std::numeric_limits<std::uint32_t>::max())) {
        return false;
    }
    target = static_cast<std::uint32_t>(value);
    return true;
}

}  // namespace

const FunctionRange *Engine::Impl::fragmentContaining(std::uint32_t root, std::uint32_t rva) const
{
    const FunctionRange *range = containing(rva);
    return range != nullptr && range->root == root ? range : nullptr;
}

Engine::Impl::RootValidation Engine::Impl::validateRoot(std::uint32_t root, ValidationBudget &budget,
                                                        BuildStats &batch) const
{
    RootValidation validation;
    const FunctionRange *primary = containing(root);
    if (primary == nullptr || primary->begin != root || primary->root != root) {
        return validation;
    }
    if (budget.roots >= kMaximumBatchFunctionValidations) {
        validation.budget_exhausted = true;
        if (!budget.root_budget_reported) {
            budget.root_budget_reported = true;
            incrementDiagnostic(batch.string_validation_budget_exhausted);
        }
        return validation;
    }
    ++budget.roots;
    incrementDiagnostic(batch.string_validation_functions);

    std::vector<const FunctionRange *> fragments;
    fragments.push_back(primary);
    if (const auto it = chained_fragment_starts.find(root); it != chained_fragment_starts.end()) {
        fragments.reserve(fragments.size() + it->second.size());
        for (const std::uint32_t start : it->second) {
            const FunctionRange *fragment = fragmentContaining(root, start);
            if (fragment == nullptr || fragment->begin != start) {
                validation.complete = false;
                return validation;
            }
            fragments.push_back(fragment);
        }
    }

    if (fragments.size() > kMaximumFunctionDecodeInstructions) {
        validation.budget_exhausted = true;
        incrementDiagnostic(batch.string_function_instruction_budget_exhausted);
        return validation;
    }

    validation.instructions.reserve(256);
    std::vector<std::uint32_t> work;
    work.reserve(fragments.size());
    for (const FunctionRange *fragment : std::views::reverse(fragments)) {
        work.push_back(fragment->begin);
    }
    std::unordered_set<std::uint32_t> visited;
    visited.reserve(kMaximumFunctionDecodeInstructions);
    bool control_flow_incomplete = false;
    std::size_t root_instructions = 0;
    std::size_t decoded_bytes = 0;

    auto hard_failure = [&]() {
        validation.instructions.clear();
        validation.eligible_targets.clear();
        validation.complete = false;
        validation.witness_valid = false;
    };

    while (!work.empty()) {
        const std::uint32_t cursor = work.back();
        work.pop_back();
        if (visited.contains(cursor)) {
            continue;
        }
        const FunctionRange *fragment = fragmentContaining(root, cursor);
        if (fragment == nullptr) {
            hard_failure();
            return validation;
        }
        visited.insert(cursor);
        if (root_instructions >= kMaximumFunctionDecodeInstructions) {
            validation.budget_exhausted = true;
            hard_failure();
            incrementDiagnostic(batch.string_function_instruction_budget_exhausted);
            return validation;
        }
        if (budget.instructions >= kMaximumBatchDecodedInstructions) {
            validation.budget_exhausted = true;
            hard_failure();
            if (!budget.instruction_budget_reported) {
                budget.instruction_budget_reported = true;
                incrementDiagnostic(batch.string_instruction_budget_exhausted);
            }
            return validation;
        }

        const std::size_t available = fragment->end - cursor;
        const auto instruction = decodeOne(image + cursor, cursor, available);
        if (!instruction) {
            hard_failure();
            return validation;
        }
        const std::uint64_t instruction_end = static_cast<std::uint64_t>(cursor) + instruction->size;
        if (instruction_end > fragment->end) {
            hard_failure();
            return validation;
        }
        if (instruction->size > kMaximumFunctionDecodeBytes - decoded_bytes) {
            validation.budget_exhausted = true;
            hard_failure();
            incrementDiagnostic(batch.string_function_byte_budget_exhausted);
            return validation;
        }
        decoded_bytes += instruction->size;
        ++root_instructions;
        ++budget.instructions;
        incrementDiagnostic(batch.decoded_instructions);
        const bool eligible = isEligibleStringLea(*instruction);
        const std::uint64_t target_wide = eligible ? INSTRUCTION_GET_RIP_TARGET(&*instruction) : 0;
        const std::uint32_t target =
            target_wide <= std::numeric_limits<std::uint32_t>::max() ? static_cast<std::uint32_t>(target_wide) : 0;
        validation.instructions.push_back({.begin = cursor,
                                           .end = static_cast<std::uint32_t>(instruction_end),
                                           .rip_target = target,
                                           .eligible = eligible});
        if (eligible) {
            validation.eligible_targets.push_back(target);
        }

        const unsigned flow = META_GET_FC(instruction->meta);
        std::vector<std::uint32_t> destinations;
        auto add_known_edge = [&](std::uint64_t destination) {
            if (destination > std::numeric_limits<std::uint32_t>::max() ||
                fragmentContaining(root, static_cast<std::uint32_t>(destination)) == nullptr) {
                control_flow_incomplete = true;
                return;
            }
            destinations.push_back(static_cast<std::uint32_t>(destination));
        };
        auto add_fallthrough = [&]() {
            if (instruction_end > fragment->end) {
                hard_failure();
                return false;
            }
            if (instruction_end == fragment->end) {
                control_flow_incomplete = true;
                return true;
            }
            destinations.push_back(static_cast<std::uint32_t>(instruction_end));
            return true;
        };

        if (flow == FC_CND_BRANCH || flow == FC_UNC_BRANCH) {
            if (instruction->opsNo == 1 && instruction->ops[0].type == O_PC) {
                add_known_edge(INSTRUCTION_GET_TARGET(&*instruction));
            }
            else {
                control_flow_incomplete = true;
            }
            if (flow == FC_CND_BRANCH && !add_fallthrough()) {
                return validation;
            }
        }
        else if (flow == FC_NONE || flow == FC_CMOV || flow == FC_CALL) {
            if (!add_fallthrough()) {
                return validation;
            }
        }
        else if (flow != FC_RET) {
            hard_failure();
            return validation;
        }

        std::ranges::sort(destinations);
        const auto duplicate = std::ranges::unique(destinations);
        destinations.erase(duplicate.begin(), duplicate.end());
        for (const std::uint32_t destination : std::views::reverse(destinations)) {
            work.push_back(destination);
        }
    }

    std::ranges::sort(validation.instructions,
                      [](const ReferenceInstruction &a, const ReferenceInstruction &b) { return a.begin < b.begin; });
    for (std::size_t i = 1; i < validation.instructions.size(); ++i) {
        if (validation.instructions[i - 1].end > validation.instructions[i].begin) {
            validation.instructions.clear();
            validation.eligible_targets.clear();
            incrementDiagnostic(batch.string_reference_overlaps);
            return validation;
        }
    }
    std::ranges::sort(validation.eligible_targets);
    const auto duplicate_targets = std::ranges::unique(validation.eligible_targets);
    validation.eligible_targets.erase(duplicate_targets.begin(), duplicate_targets.end());
    validation.witness_valid = true;
    validation.complete = !control_flow_incomplete;
    return validation;
}

std::vector<Engine::Impl::StringCandidate> Engine::Impl::decodeStrings(
    std::uint32_t root, BuildStats &batch, std::map<std::uint32_t, RootValidation> &validations,
    ValidationBudget &budget) const
{
    auto validation_it = validations.find(root);
    if (validation_it == validations.end()) {
        const std::size_t roots_before = budget.roots;
        RootValidation validation = validateRoot(root, budget, batch);
        if (budget.roots == roots_before) {
            return {};
        }
        validation_it = validations.emplace(root, std::move(validation)).first;
    }
    const RootValidation &validation = validation_it->second;
    if (!validation.witness_valid) {
        return {};
    }

    std::map<std::uint32_t, StringCandidate> candidates;
    for (const ReferenceInstruction &instruction : validation.instructions) {
        if (!instruction.eligible) {
            continue;
        }
        const Section *target_section = sectionContaining(instruction.rip_target);
        if (target_section == nullptr || target_section->executable) {
            continue;
        }
        std::string value = readCString(instruction.rip_target, 180);
        const int score = ::spark::symbol_guess::windows::scoreStringHint(value);
        if (score < ::spark::symbol_guess::kMinimumStringHintScore) {
            continue;
        }
        candidates.try_emplace(
            instruction.rip_target,
            StringCandidate{.target = instruction.rip_target, .value = std::move(value), .score = score});
    }

    std::vector<StringCandidate> out;
    out.reserve(candidates.size());
    for (auto &[target, candidate] : candidates) {
        out.push_back(std::move(candidate));
    }
    std::ranges::sort(out, [](const StringCandidate &a, const StringCandidate &b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        if (a.value != b.value) {
            return a.value < b.value;
        }
        return a.target < b.target;
    });
    for (std::size_t i = 0; i < out.size(); ++i) {
        incrementDiagnostic(batch.string_candidates);
    }
    return out;
}

void Engine::Impl::scanCandidateReferences(const std::unordered_set<std::uint32_t> &targets,
                                           std::map<std::uint32_t, std::set<std::uint32_t>> &references,
                                           std::map<std::uint32_t, RootValidation> &validations,
                                           ValidationBudget &budget, BuildStats &batch) const
{
    batch.string_reference_candidates = std::min<std::size_t>(targets.size(), KMaximumDiagnosticCounter);
    enum class ReferenceState {
        Active,
        Ambiguous,
        Shared,
    };
    std::unordered_map<std::uint32_t, ReferenceState> states;
    states.reserve(targets.size());
    std::size_t active_targets = targets.size();
    for (const std::uint32_t target : targets) {
        const auto existing = references.find(target);
        if (existing != references.end() && existing->second.size() >= 2) {
            states.emplace(target, ReferenceState::Shared);
            --active_targets;
            incrementDiagnostic(batch.string_reference_shared);
        }
        else {
            states.emplace(target, ReferenceState::Active);
        }
    }

    enum class AmbiguityReason {
        Unindexed,
        Unreachable,
        Invalid,
        Budget,
    };
    const auto mark_ambiguous = [&](std::uint32_t target, AmbiguityReason reason) {
        const auto state = states.find(target);
        if (state == states.end() || state->second != ReferenceState::Active) {
            return active_targets == 0;
        }
        state->second = ReferenceState::Ambiguous;
        --active_targets;
        references.erase(target);
        incrementDiagnostic(batch.string_reference_ambiguities);
        if (reason == AmbiguityReason::Unindexed) {
            incrementDiagnostic(batch.string_reference_unindexed);
        }
        else if (reason == AmbiguityReason::Unreachable || reason == AmbiguityReason::Invalid) {
            incrementDiagnostic(batch.string_reference_unreachable);
        }
        return active_targets == 0;
    };

    const auto record_owner = [&](std::uint32_t target, std::uint32_t root) {
        const auto state = states.find(target);
        if (state == states.end() || state->second != ReferenceState::Active) {
            return active_targets == 0;
        }
        auto &owners = references[target];
        if (owners.insert(root).second && owners.size() == 2) {
            state->second = ReferenceState::Shared;
            --active_targets;
            incrementDiagnostic(batch.string_reference_shared);
        }
        return active_targets == 0;
    };

    const auto find_instruction = [](const std::vector<ReferenceInstruction> &instructions, std::uint32_t rva) {
        const auto it = std::ranges::upper_bound(instructions, rva, std::ranges::less{}, &ReferenceInstruction::begin);
        if (it == instructions.begin()) {
            return static_cast<const ReferenceInstruction *>(nullptr);
        }
        const auto &instruction = *std::prev(it);
        return rva < instruction.end ? &instruction : nullptr;
    };

    const auto classify = [&](std::uint32_t target, std::uint32_t rva) {
        const auto state = states.find(target);
        if (state == states.end()) {
            return active_targets == 0;
        }
        if (state->second != ReferenceState::Active) {
            incrementDiagnostic(batch.string_reference_terminal_hits_skipped);
            return active_targets == 0;
        }
        const FunctionRange *function = containing(rva);
        const FunctionRange *primary = function != nullptr ? containing(function->root) : nullptr;
        if (function == nullptr || primary == nullptr || primary->begin != function->root ||
            primary->root != function->root) {
            return mark_ambiguous(target, AmbiguityReason::Unindexed);
        }
        const auto existing = references.find(target);
        if (existing != references.end() && existing->second.contains(function->root)) {
            return active_targets == 0;
        }
        auto validation_it = validations.find(function->root);
        RootValidation uncached_validation;
        const RootValidation *validation = nullptr;
        if (validation_it == validations.end()) {
            const std::size_t roots_before = budget.roots;
            uncached_validation = validateRoot(function->root, budget, batch);
            if (budget.roots != roots_before) {
                const auto inserted = validations.emplace(function->root, std::move(uncached_validation));
                validation_it = inserted.first;
                validation = &validation_it->second;
            }
            else {
                validation = &uncached_validation;
            }
        }
        else {
            validation = &validation_it->second;
        }
        if (!validation->witness_valid) {
            return mark_ambiguous(target,
                                  validation->budget_exhausted ? AmbiguityReason::Budget : AmbiguityReason::Invalid);
        }

        const ReferenceInstruction *instruction = find_instruction(validation->instructions, rva);
        if (instruction != nullptr && instruction->eligible && instruction->rip_target == target) {
            incrementDiagnostic(batch.string_reference_exact_hits);
            return record_owner(target, function->root);
        }
        if (validation->complete) {
            if (instruction != nullptr) {
                if (rva > instruction->begin) {
                    incrementDiagnostic(batch.string_reference_interior_rejections);
                }
                return active_targets == 0;
            }
            return mark_ambiguous(target, AmbiguityReason::Unreachable);
        }
        return mark_ambiguous(target, AmbiguityReason::Unreachable);
    };

    std::size_t scanned_bytes = 0;
    bool scan_truncated = false;
    for (const Section &section : sections) {
        if (active_targets == 0) {
            break;
        }
        if (!section.executable || section.end <= section.begin) {
            continue;
        }
        if (scanned_bytes >= kMaximumBatchExecutableScanBytes) {
            scan_truncated = true;
            break;
        }
        const std::size_t available = section.end - section.begin;
        const std::size_t remaining = kMaximumBatchExecutableScanBytes - scanned_bytes;
        const std::size_t length = std::min(available, remaining);
        scanned_bytes += length;
        if (length < available) {
            scan_truncated = true;
        }
        if (length < 6) {
            continue;
        }
        const std::uint8_t *bytes = image + section.begin;
        const std::uint8_t *cursor = bytes;
        const std::uint8_t *end = bytes + length;
        while (static_cast<std::size_t>(end - cursor) >= 6) {
            const auto remaining_bytes = static_cast<std::size_t>(end - cursor);
            const auto *opcode = static_cast<const std::uint8_t *>(std::memchr(cursor, 0x8d, remaining_bytes));
            if (opcode == nullptr || static_cast<std::size_t>(end - opcode) < 6) {
                break;
            }
            cursor = opcode + 1;
            if ((opcode[1] & 0xc7) != 0x05) {
                continue;
            }
            std::int32_t displacement = 0;
            std::memcpy(&displacement, opcode + 2, sizeof(displacement));
            const auto opcode_rva = section.begin + static_cast<std::uint32_t>(opcode - bytes);
            std::uint32_t target = 0;
            if (!addRipDisplacement(opcode_rva, displacement, target) || !targets.contains(target)) {
                continue;
            }
            incrementDiagnostic(batch.string_reference_potential_hits);
            classify(target, opcode_rva);
        }
    }

    if (scan_truncated) {
        incrementDiagnostic(batch.string_scan_byte_budget_exhausted);
        for (const std::uint32_t target : targets) {
            const auto state = states.find(target);
            if (state != states.end() && state->second == ReferenceState::Active) {
                mark_ambiguous(target, AmbiguityReason::Budget);
            }
        }
    }
}

}  // namespace spark::symbol_guess::windows

#endif
