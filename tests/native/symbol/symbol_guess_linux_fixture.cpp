#include "native/symbol/symbol_guess.h"

#if defined(__linux__) && defined(__x86_64__)

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <typeinfo>
#include <unordered_map>
#include <vector>

#include "native/symbol/symbol_guess_linux.h"

extern "C" std::uintptr_t sparkFixtureStringTarget();
extern "C" std::uintptr_t sparkFixturePrefixedStringTarget();
extern "C" std::uintptr_t sparkFixtureUniqueStringTarget();
extern "C" std::uintptr_t sparkFixtureMalformedStringTarget();
extern "C" std::uintptr_t sparkFixtureSyscallStringTarget();
extern "C" std::uintptr_t sparkFixtureTruncatedStringTarget();
extern "C" std::uintptr_t sparkFixtureOtherIndirectTarget();
extern "C" std::uintptr_t sparkFixtureCallTailStringTarget();
extern "C" std::uintptr_t sparkFixtureTruncatedCallStringTarget();
extern "C" std::uintptr_t sparkFixtureOtherCallStringTarget();
extern "C" std::uintptr_t sparkFixtureEmbeddedStringTarget();
extern "C" std::uintptr_t sparkFixtureUnreachableStringTarget();
extern "C" std::uintptr_t sparkFixtureOverlapStringTarget();
extern "C" std::uintptr_t sparkFixtureUnindexedStringTarget();
extern "C" std::uintptr_t sparkFixtureBudgetStringTarget();
extern "C" std::uintptr_t sparkFixtureLargeStringTarget();
extern "C" std::uintptr_t sparkFixtureFunctionBudgetTarget();
extern "C" std::uintptr_t sparkFixtureBatchBudgetTarget();
extern "C" std::uintptr_t sparkFixtureLateStringTarget();
extern "C" std::uintptr_t sparkFixtureWeakTarget();
extern "C" std::uintptr_t sparkFixtureWeakSharedTarget();
extern "C" std::uintptr_t sparkFixtureWeakOtherTarget();
extern "C" std::uintptr_t sparkFixtureWeakAmbiguousTarget();
extern "C" std::uintptr_t sparkFixtureInteriorThunk();

namespace fixture {

class VtableOwner {
public:
    [[nodiscard]] virtual int run() const;
};

__attribute__((noinline, used)) int VtableOwner::run() const
{
    return 17;
}

static VtableOwner owner;

// Itanium vtable metadata followed by an interior target.
__attribute__((used, visibility("hidden"))) static const void *interior_vtable[] = {
    nullptr,
    &typeid(VtableOwner),
    // This deliberately points one byte into a function for the invalid fixture case.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    reinterpret_cast<const void *>(reinterpret_cast<std::uintptr_t>(&sparkFixtureUniqueStringTarget) + 1),
    nullptr,
};

}  // namespace fixture

namespace {

std::uint64_t imageBase()
{
    Dl_info info{};
    if (::dladdr(reinterpret_cast<void *>(&sparkFixtureUniqueStringTarget), &info) == 0 || info.dli_fbase == nullptr) {
        return 0;
    }
    return reinterpret_cast<std::uint64_t>(info.dli_fbase);
}

std::uint64_t functionRva(const void *address, std::uint64_t base)
{
    const auto value = reinterpret_cast<std::uint64_t>(address);
    return value >= base ? value - base : 0;
}

const spark::GuessResult *findResult(std::uint64_t query,
                                     const std::unordered_map<std::uint64_t, spark::GuessResult> &results)
{
    const auto it = results.find(query);
    return it == results.end() ? nullptr : &it->second;
}

void printResult(const char *name, std::uint64_t query,
                 const std::unordered_map<std::uint64_t, spark::GuessResult> &results)
{
    const auto *result = findResult(query, results);
    std::printf("%s_label=%s\n%s_root=%llu\n", name, result == nullptr ? "" : result->label.c_str(), name,
                static_cast<unsigned long long>(result == nullptr ? 0 : result->function_rva));
}

std::unordered_map<std::uint64_t, spark::GuessResult> analyze(std::initializer_list<std::uint64_t> rvas)
{
    return spark::analyzeMainModuleSymbols(std::vector<std::uint64_t>(rvas));
}

}  // namespace

int main()
{
    const std::uint64_t base = imageBase();
    if (base == 0) {
        return 2;
    }

    const std::uint64_t shared_rva = functionRva(reinterpret_cast<void *>(&sparkFixtureStringTarget), base);
    const std::uint64_t prefixed_rva = functionRva(reinterpret_cast<void *>(&sparkFixturePrefixedStringTarget), base);
    const std::uint64_t unique_rva = functionRva(reinterpret_cast<void *>(&sparkFixtureUniqueStringTarget), base);
    const std::uint64_t malformed_rva = functionRva(reinterpret_cast<void *>(&sparkFixtureMalformedStringTarget), base);
    const std::uint64_t syscall_rva = functionRva(reinterpret_cast<void *>(&sparkFixtureSyscallStringTarget), base);
    const std::uint64_t truncated_rva = functionRva(reinterpret_cast<void *>(&sparkFixtureTruncatedStringTarget), base);
    const std::uint64_t other_indirect_rva =
        functionRva(reinterpret_cast<void *>(&sparkFixtureOtherIndirectTarget), base);
    const std::uint64_t call_tail_rva = functionRva(reinterpret_cast<void *>(&sparkFixtureCallTailStringTarget), base);
    const std::uint64_t truncated_call_rva =
        functionRva(reinterpret_cast<void *>(&sparkFixtureTruncatedCallStringTarget), base);
    const std::uint64_t other_call_rva =
        functionRva(reinterpret_cast<void *>(&sparkFixtureOtherCallStringTarget), base);
    const std::uint64_t embedded_rva = functionRva(reinterpret_cast<void *>(&sparkFixtureEmbeddedStringTarget), base);
    const std::uint64_t unreachable_rva =
        functionRva(reinterpret_cast<void *>(&sparkFixtureUnreachableStringTarget), base);
    const std::uint64_t overlap_rva = functionRva(reinterpret_cast<void *>(&sparkFixtureOverlapStringTarget), base);
    const std::uint64_t unindexed_rva = functionRva(reinterpret_cast<void *>(&sparkFixtureUnindexedStringTarget), base);
    const std::uint64_t budget_rva = functionRva(reinterpret_cast<void *>(&sparkFixtureBudgetStringTarget), base);
    const std::uint64_t large_rva = functionRva(reinterpret_cast<void *>(&sparkFixtureLargeStringTarget), base);
    const std::uint64_t function_budget_rva =
        functionRva(reinterpret_cast<void *>(&sparkFixtureFunctionBudgetTarget), base);
    const std::uint64_t batch_budget_rva = functionRva(reinterpret_cast<void *>(&sparkFixtureBatchBudgetTarget), base);
    const std::uint64_t late_rva = functionRva(reinterpret_cast<void *>(&sparkFixtureLateStringTarget), base);
    const std::uint64_t weak_rva = functionRva(reinterpret_cast<void *>(&sparkFixtureWeakTarget), base);
    const std::uint64_t weak_shared_rva = functionRva(reinterpret_cast<void *>(&sparkFixtureWeakSharedTarget), base);
    const std::uint64_t weak_other_rva = functionRva(reinterpret_cast<void *>(&sparkFixtureWeakOtherTarget), base);
    const std::uint64_t weak_ambiguous_rva =
        functionRva(reinterpret_cast<void *>(&sparkFixtureWeakAmbiguousTarget), base);
    const std::uint64_t thunk_rva = functionRva(reinterpret_cast<void *>(&sparkFixtureInteriorThunk), base);

    void **vtable = *reinterpret_cast<void ***>(&fixture::owner);
    const std::uint64_t vtable_rva = functionRva(vtable[0], base);

    // The shared pair is negative even when queried alone; query order must not change it.
    printResult("shared_single", shared_rva, analyze({shared_rva}));
    printResult("prefixed_single", prefixed_rva, analyze({prefixed_rva}));
    printResult("shared_pair", shared_rva, analyze({shared_rva, prefixed_rva}));
    printResult("prefixed_pair", prefixed_rva, analyze({prefixed_rva, shared_rva}));
    printResult("shared_unique", shared_rva, analyze({shared_rva, unique_rva}));
    const auto shared_stats = spark::symbol_guess::linux::currentModuleStats();

    printResult("unique", unique_rva, analyze({unique_rva}));
    printResult("owner_indirect", malformed_rva, analyze({malformed_rva}));
    printResult("syscall", syscall_rva, analyze({syscall_rva}));
    printResult("truncated", truncated_rva, analyze({truncated_rva}));
    printResult("other_indirect", other_indirect_rva, analyze({other_indirect_rva}));
    printResult("call_tail", call_tail_rva, analyze({call_tail_rva}));
    printResult("truncated_call", truncated_call_rva, analyze({truncated_call_rva}));
    printResult("other_call", other_call_rva, analyze({other_call_rva}));
    printResult("embedded", embedded_rva, analyze({embedded_rva}));
    const auto embedded_stats = spark::symbol_guess::linux::currentModuleStats();
    printResult("unreachable", unreachable_rva, analyze({unreachable_rva}));
    const auto unreachable_stats = spark::symbol_guess::linux::currentModuleStats();
    printResult("overlap", overlap_rva, analyze({overlap_rva}));
    const auto overlap_stats = spark::symbol_guess::linux::currentModuleStats();
    const auto unindexed_query = analyze({unindexed_rva, late_rva});
    printResult("unindexed", unindexed_rva, unindexed_query);
    printResult("late", late_rva, unindexed_query);
    const auto unindexed_stats = spark::symbol_guess::linux::currentModuleStats();
    printResult("budget", budget_rva, analyze({budget_rva}));
    const auto cache_stats = spark::symbol_guess::linux::currentModuleStats();
    printResult("large", large_rva, analyze({large_rva}));
    const auto large_stats = spark::symbol_guess::linux::currentModuleStats();
    printResult("function_budget", function_budget_rva, analyze({function_budget_rva}));
    const auto function_stats = spark::symbol_guess::linux::currentModuleStats();
    printResult("batch_budget", batch_budget_rva, analyze({batch_budget_rva}));
    const auto batch_stats = spark::symbol_guess::linux::currentModuleStats();

    printResult("weak", weak_rva, analyze({weak_rva}));
    printResult("weak_shared", weak_shared_rva, analyze({weak_shared_rva}));
    printResult("weak_other", weak_other_rva, analyze({weak_other_rva}));
    printResult("weak_ambiguous", weak_ambiguous_rva, analyze({weak_ambiguous_rva}));

    printResult("vtable", vtable_rva, analyze({vtable_rva}));
    printResult("thunk", thunk_rva, analyze({thunk_rva}));
    const auto thunk_stats = spark::symbol_guess::linux::currentModuleStats();

    std::printf("shared_rva=%llu\nprefixed_rva=%llu\nunique_rva=%llu\nmalformed_rva=%llu\n"
                "truncated_rva=%llu\nother_indirect_rva=%llu\ncall_tail_rva=%llu\n"
                "truncated_call_rva=%llu\nother_call_rva=%llu\nsyscall_rva=%llu\nembedded_rva=%llu\n"
                "unreachable_rva=%llu\noverlap_rva=%llu\nunindexed_rva=%llu\nbudget_rva=%llu\nlarge_rva=%llu\n"
                "function_budget_rva=%llu\nbatch_budget_rva=%llu\nweak_rva=%llu\n"
                "late_rva=%llu\nweak_shared_rva=%llu\nweak_other_rva=%llu\nweak_ambiguous_rva=%llu\n"
                "vtable_rva=%llu\n"
                "thunk_rva=%llu\n",
                static_cast<unsigned long long>(shared_rva), static_cast<unsigned long long>(prefixed_rva),
                static_cast<unsigned long long>(unique_rva), static_cast<unsigned long long>(malformed_rva),
                static_cast<unsigned long long>(truncated_rva), static_cast<unsigned long long>(other_indirect_rva),
                static_cast<unsigned long long>(call_tail_rva), static_cast<unsigned long long>(truncated_call_rva),
                static_cast<unsigned long long>(other_call_rva), static_cast<unsigned long long>(syscall_rva),
                static_cast<unsigned long long>(embedded_rva), static_cast<unsigned long long>(unreachable_rva),
                static_cast<unsigned long long>(overlap_rva), static_cast<unsigned long long>(unindexed_rva),
                static_cast<unsigned long long>(budget_rva), static_cast<unsigned long long>(large_rva),
                static_cast<unsigned long long>(function_budget_rva), static_cast<unsigned long long>(batch_budget_rva),
                static_cast<unsigned long long>(weak_rva), static_cast<unsigned long long>(late_rva),
                static_cast<unsigned long long>(weak_shared_rva), static_cast<unsigned long long>(weak_other_rva),
                static_cast<unsigned long long>(weak_ambiguous_rva), static_cast<unsigned long long>(vtable_rva),
                static_cast<unsigned long long>(thunk_rva));
    const auto print_stats = [](const char *prefix, const spark::symbol_guess::linux::BuildStats &stats) {
        std::printf("%s_vtable_interior_target_rejections=%llu\n%s_thunk_interior_destination_rejections=%llu\n"
                    "%s_string_reference_exact_hits=%llu\n%s_string_reference_interior_rejections=%llu\n"
                    "%s_string_reference_ambiguities=%llu\n%s_string_reference_shared=%llu\n"
                    "%s_string_reference_terminal_hits_skipped=%llu\n%s_string_reference_unindexed=%llu\n"
                    "%s_string_reference_unreachable=%llu\n%s_string_reference_overlaps=%llu\n"
                    "%s_string_validation_functions=%llu\n%s_string_validation_budget_exhausted=%llu\n"
                    "%s_string_function_byte_budget_exhausted=%llu\n"
                    "%s_string_function_instruction_budget_exhausted=%llu\n"
                    "%s_string_instruction_budget_exhausted=%llu\n",
                    prefix, static_cast<unsigned long long>(stats.vtable_interior_target_rejections), prefix,
                    static_cast<unsigned long long>(stats.thunk_interior_destination_rejections), prefix,
                    static_cast<unsigned long long>(stats.string_reference_exact_hits), prefix,
                    static_cast<unsigned long long>(stats.string_reference_interior_rejections), prefix,
                    static_cast<unsigned long long>(stats.string_reference_ambiguities), prefix,
                    static_cast<unsigned long long>(stats.string_reference_shared), prefix,
                    static_cast<unsigned long long>(stats.string_reference_terminal_hits_skipped), prefix,
                    static_cast<unsigned long long>(stats.string_reference_unindexed), prefix,
                    static_cast<unsigned long long>(stats.string_reference_unreachable), prefix,
                    static_cast<unsigned long long>(stats.string_reference_overlaps), prefix,
                    static_cast<unsigned long long>(stats.string_validation_functions), prefix,
                    static_cast<unsigned long long>(stats.string_validation_budget_exhausted), prefix,
                    static_cast<unsigned long long>(stats.string_function_byte_budget_exhausted), prefix,
                    static_cast<unsigned long long>(stats.string_function_instruction_budget_exhausted), prefix,
                    static_cast<unsigned long long>(stats.string_instruction_budget_exhausted));
    };
    print_stats("cache", cache_stats);
    print_stats("shared", shared_stats);
    print_stats("embedded", embedded_stats);
    print_stats("unreachable", unreachable_stats);
    print_stats("overlap", overlap_stats);
    print_stats("unindexed", unindexed_stats);
    print_stats("large", large_stats);
    print_stats("function", function_stats);
    print_stats("batch", batch_stats);
    print_stats("thunk", thunk_stats);
    return 0;
}

#endif
