#include "native/symbol/symbol_guess_linux.h"

#if defined(__linux__) && defined(__x86_64__)

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <optional>
#include <unordered_map>

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

namespace {

void checkPattern(std::vector<std::uint8_t> code, std::string_view expected)
{
    for (const auto base : {0x100ULL, 0x12345000ULL}) {
        for (bool reverse : {false, true}) {
            std::size_t count = 0;
            const auto result = spark::symbol_guess::linux::decodeCodePattern(code, base, &count, reverse);
            CHECK(result.label == expected);
            CHECK(count <= code.size());
            if (!result.empty()) {
                CHECK(result.kind == spark::GuessKind::Type);
                CHECK(result.confidence == spark::Confidence::Medium);
            }
        }
    }
}

class LambdaImage : public spark::symbol_guess::dwarf::ImageView {
public:
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x2000, 0xc3);
    bool fail_read = false;
    mutable std::size_t reads = 0;
    mutable std::size_t read_bytes = 0;

    bool read(std::uint64_t rva, void *out, std::size_t length) const override
    {
        ++reads;
        read_bytes += length;
        if (fail_read || !executable(rva, length)) {
            return false;
        }
        std::memcpy(out, bytes.data() + rva, length);
        return true;
    }
    [[nodiscard]] bool executable(std::uint64_t rva, std::size_t length) const override
    {
        return rva < bytes.size() && length <= bytes.size() - rva;
    }
    [[nodiscard]] std::uint64_t readableEnd(std::uint64_t rva) const override
    {
        return rva < bytes.size() ? bytes.size() : 0;
    }

    void calls(std::uint64_t root, std::initializer_list<std::uint64_t> targets)
    {
        for (auto target : targets) {
            bytes[root] = 0xe8;
            const auto displacement = static_cast<std::int32_t>(target - root - 5);
            std::memcpy(bytes.data() + root + 1, &displacement, sizeof(displacement));
            root += 5;
        }
        bytes[root] = 0xc3;
    }
};

void checkLambdaIndex()
{
    namespace dwarf = spark::symbol_guess::dwarf;
    namespace linux = spark::symbol_guess::linux;
    using spark::symbol_guess::TypedLabel;
    const std::vector<dwarf::FunctionRange> ranges = {{.begin = 0x100, .end = 0x120, .root = 0x100},
                                                      {.begin = 0x200, .end = 0x220, .root = 0x200},
                                                      {.begin = 0x500, .end = 0x800, .root = 0x500},
                                                      {.begin = 0x900, .end = 0xc00, .root = 0x900}};
    const auto owner = [](std::string_view name) {
        return TypedLabel{.label = "vtable: std::__1::function::__func<" + std::string(name) + ", A>::vfn[6]",
                          .kind = spark::GuessKind::Vtable,
                          .confidence = spark::Confidence::High};
    };
    for (bool reverse : {false, true}) {
        for (const auto operand : {0xd0, 0x10}) {
            LambdaImage image;
            std::unordered_map<std::uint64_t, TypedLabel> labels = {{0x100, owner("Lambda")}, {0x200, owner("Lambda")}};
            const auto first = reverse ? labels.begin()->first : std::next(labels.begin())->first;
            const auto second = reverse ? std::next(labels.begin())->first : labels.begin()->first;
            image.calls(first, {0x500});
            image.bytes[second] = 0xff;
            image.bytes[second + 1] = operand;
            labels.at(second) = owner("Other");
            const auto index = linux::collectLambdaBodyIndex(image, ranges, labels);
            CHECK(!index.complete);
            CHECK(index.labels.empty());
            CHECK(linux::projectLambdaBodyLabel(index, 0x500).empty());
            CHECK(linux::projectLambdaBodyLabel(index, 0x500, owner("Earlier")).label == owner("Earlier").label);
        }
        for (bool same_owner : {false, true}) {
            LambdaImage image;
            const auto first = reverse ? 0x200 : 0x100;
            const auto second = reverse ? 0x100 : 0x200;
            image.calls(first, {0x500, 0x501});
            image.calls(second, {0x500, 0x900});
            std::unordered_map<std::uint64_t, TypedLabel> labels;
            labels.emplace(second, owner(same_owner ? "Lambda" : "Other"));
            labels.emplace(first, owner("Lambda"));
            std::size_t count = 0;
            const auto index = linux::collectLambdaBodyIndex(image, ranges, labels, &count);
            CHECK(index.complete);
            CHECK(index.wrappers.size() == 2);
            CHECK(count == 6);
            CHECK(image.reads == count);
            CHECK(image.read_bytes <= count * 15);
            const std::string_view expected = same_owner ? "call?: Lambda (lambda body)" : "";
            for (const std::vector<std::uint64_t> &requests : {std::vector<std::uint64_t>{0x500},
                                                               {0x100, 0x500},
                                                               {0x100, 0x500, 0x900},
                                                               {0x900, 0x500, 0x100, 0x500}}) {
                for (auto request : requests) {
                    if (request == 0x500) {
                        CHECK(linux::projectLambdaBodyLabel(index, request).label == expected);
                    }
                }
            }
            CHECK(count == 6);
            CHECK(image.reads == count);
            CHECK(linux::projectLambdaBodyLabel(index, 0x500, owner("Earlier")).label == owner("Earlier").label);
            image.fail_read = true;
            CHECK(!linux::collectLambdaBodyIndex(image, ranges, labels).complete);
            image.fail_read = false;
            image.bytes[second + 10] = 0xff;
            image.bytes[second + 11] = 0xe0;
            const auto failed = linux::collectLambdaBodyIndex(image, ranges, labels);
            CHECK(!failed.complete);
            CHECK(failed.labels.empty());
        }
    }
    LambdaImage image;
    image.calls(0x100, {0x500, 0x900});
    auto index = linux::collectLambdaBodyIndex(image, ranges, {{0x100, owner("Lambda")}});
    CHECK(index.complete);
    CHECK(linux::projectLambdaBodyLabel(index, 0x500).empty());
    const auto with_earlier =
        linux::collectLambdaBodyIndex(image, ranges, {{0x100, owner("Lambda")}, {0x900, owner("Earlier")}});
    CHECK(with_earlier.complete);
    CHECK(linux::projectLambdaBodyLabel(with_earlier, 0x500).empty());
    image.calls(0x100, {0x500});
    index = linux::collectLambdaBodyIndex(image, ranges, {{0x100, owner("Lambda")}});
    CHECK(linux::projectLambdaBodyLabel(index, 0x500).label == "call?: Lambda (lambda body)");
    image.calls(0x200, {0x500});
    index = linux::collectLambdaBodyIndex(image, ranges, {{0x100, owner("Lambda")}, {0x200, owner("Other")}});
    CHECK(linux::projectLambdaBodyLabel(index, 0x500).empty());
}

}  // namespace

int main()
{
    checkPattern({0x48, 0xb8, 0xf0, 0x0f, 0xb1, 0, 0, 0, 0, 0, 0xc3}, "");
    checkPattern({0xf0, 0x0f, 0xb1, 0x07, 0xc3}, "");
    checkPattern({0xf0, 0x0f, 0xb1, 0x07, 0x75, 0xfa, 0xc3}, "type?: atomic_cas_loop (lock cmpxchg)");
    checkPattern({0xf0, 0x0f, 0xb1, 0x07, 0x90, 0x75, 0xfd, 0xc3}, "");
    checkPattern({0x69, 0xc0, 0xb9, 0x79, 0x37, 0x9e, 0xc3}, "type?: hash_table_lookup (Knuth multiplicative hash)");
    checkPattern({0xb8, 0xb9, 0x79, 0x37, 0x9e, 0xc3}, "");
    checkPattern({0x48, 0x69, 0xc0, 0xb9, 0x79, 0x37, 0x9e, 0xc3}, "");
    checkPattern({0x69, 0xc0, 0xb9, 0x79, 0x37}, "");
    checkPattern({0xeb, 0x06, 0x69, 0xc0, 0xb9, 0x79, 0x37, 0x9e, 0xc3}, "");
    checkPattern({0x75, 0x02, 0x69, 0xc0, 0xb9, 0x79, 0x37, 0x9e, 0xc3}, "");
    checkPattern({0x48, 0xb9, 0x69, 0x2d, 0x38, 0xeb, 0x08, 0xea, 0xdf, 0x9d, 0x48, 0x0f, 0xaf, 0xc1, 0xc3},
                 "type?: hash_table_lookup (64-bit hash multiplier)");
    checkPattern({0x48, 0xb9, 0x69, 0x2d, 0x38, 0xeb, 0x08, 0xea, 0xdf, 0x9d, 0xc3}, "");
    checkPattern({0x48, 0xb9, 0x69, 0x2d, 0x38, 0xeb, 0x08, 0xea, 0xdf, 0x9d, 0x48, 0xf7, 0xe1, 0xc3},
                 "type?: hash_table_lookup (64-bit hash multiplier)");
    checkPattern({0x48, 0xb9, 0x69, 0x2d, 0x38, 0xeb, 0x08, 0xea, 0xdf, 0x9d, 0x48, 0x6b, 0xc1, 0x03, 0xc3},
                 "type?: hash_table_lookup (64-bit hash multiplier)");
    checkPattern({0x48, 0xb9, 0x69, 0x2d, 0x38, 0xeb, 0x08, 0xea, 0xdf, 0x9d, 0x48, 0x6b, 0xca, 0x03, 0xc3}, "");
    checkPattern({0x48, 0xb9, 0x69, 0x2d, 0x38, 0xeb, 0x08, 0xea, 0xdf, 0x9d, 0x48, 0x0f, 0xaf, 0xc2, 0xc3}, "");
    checkPattern({0x48, 0xb9, 0x69, 0x2d, 0x38, 0xeb, 0x08, 0xea, 0xdf, 0x9d, 0x31, 0xc9, 0x48, 0x0f, 0xaf, 0xc1, 0xc3},
                 "");
    checkPattern({0x75, 0x0a, 0x48, 0xb9, 0x69, 0x2d, 0x38, 0xeb, 0x08, 0xea, 0xdf, 0x9d, 0x48, 0x0f, 0xaf, 0xc1, 0xc3},
                 "");
    checkPattern({0xf0, 0x0f, 0xc1, 0x07, 0xc3}, "type?: atomic_fetch_add (lock xadd)");
    checkPattern({0xf0, 0x48, 0x0f, 0xb1, 0x07, 0x75, 0xf9, 0xc3}, "type?: atomic_cas_loop (lock cmpxchg)");
    checkPattern({0xf0, 0x48, 0x0f, 0xc1, 0x07, 0xc3}, "type?: atomic_fetch_add (lock xadd)");
    checkPattern({0x69, 0xc0, 0xb9, 0x79, 0x37, 0x9e, 0x0f}, "");
    checkPattern({0x69, 0xc0, 0xb9, 0x79, 0x37, 0x9e, 0x0f, 0x05, 0xc3}, "");
    checkPattern({0x0f, 0xc1, 0x07, 0xc3}, "");
    checkPattern({0xf0, 0x0f, 0xc1, 0xc7, 0xc3}, "");
    checkPattern({0x48, 0xb8, 0xf0, 0x0f, 0xc1, 0x07, 0, 0, 0, 0, 0xc3}, "");
    checkPattern({0xd1, 0xe8, 0xd1, 0xf8, 0x48, 0xf7, 0xd0, 0xc3}, "type?: binary_search (shift-right halving)");
    checkPattern({0x48, 0xb8, 0xd1, 0xe8, 0xd1, 0xf8, 0x48, 0xf7, 0xd0, 0, 0xc3}, "");
    checkLambdaIndex();
    constexpr std::uint64_t base = 0x100;
    constexpr std::array<std::uint8_t, 8> direct = {0x48, 0x8d, 0x05, 0xf9, 0x00, 0x00, 0x00, 0xc3};
    CHECK(spark::symbol_guess::linux::decodeRipRelativeLeaTargets(direct, base) == std::vector<std::uint64_t>{0x200});

    // Only a 64-bit destination register with RIP-relative addressing is an
    // eligible string reference.
    constexpr std::array<std::uint8_t, 8> lea32 = {0x8d, 0x05, 0xfb, 0x00, 0x00, 0x00, 0xc3, 0x90};
    CHECK(spark::symbol_guess::linux::decodeRipRelativeLeaTargets(lea32, base).empty());
    constexpr std::array<std::uint8_t, 9> operand_override = {0x66, 0x48, 0x8d, 0x05, 0xf8, 0x00, 0x00, 0x00, 0xc3};
    CHECK(spark::symbol_guess::linux::decodeRipRelativeLeaTargets(operand_override, base) ==
          std::vector<std::uint64_t>{0x200});
    constexpr std::array<std::uint8_t, 9> eip_override = {0x67, 0x48, 0x8d, 0x05, 0xf8, 0x00, 0x00, 0x00, 0xc3};
    CHECK(spark::symbol_guess::linux::decodeRipRelativeLeaTargets(eip_override, base).empty());
    constexpr std::array<std::uint8_t, 8> mov = {0x48, 0x8b, 0x05, 0xf9, 0x00, 0x00, 0x00, 0xc3};
    CHECK(spark::symbol_guess::linux::decodeRipRelativeLeaTargets(mov, base).empty());

    // The LEA-looking bytes are the immediate of one MOVABS instruction and
    // must never be treated as an instruction boundary.
    constexpr std::array<std::uint8_t, 11> embedded = {0x48, 0xb8, 0x48, 0x8d, 0x05, 0xf2,
                                                       0x00, 0x00, 0x00, 0x90, 0xc3};
    CHECK(spark::symbol_guess::linux::decodeRipRelativeLeaTargets(embedded, base).empty());

    // Follow the taken edge of a conditional branch as well as fallthrough.
    constexpr std::array<std::uint8_t, 12> branch = {0x75, 0x02, 0xc3, 0x90, 0x48, 0x8d,
                                                     0x05, 0xf5, 0x00, 0x00, 0x00, 0xc3};
    CHECK(spark::symbol_guess::linux::decodeRipRelativeLeaTargets(branch, base) == std::vector<std::uint64_t>{0x200});

    // Bytes skipped by an unconditional branch are unreachable data.
    constexpr std::array<std::uint8_t, 11> unreachable = {0xeb, 0x08, 0x48, 0x8d, 0x05, 0xf2,
                                                          0x00, 0x00, 0x00, 0x90, 0xc3};
    CHECK(spark::symbol_guess::linux::decodeRipRelativeLeaTargets(unreachable, base).empty());
    CHECK(spark::symbol_guess::linux::decodeRipRelativeLeaTargets({}, base).empty());

    constexpr std::array<std::uint8_t, 5> direct_thunk = {0xe9, 0xfb, 0x00, 0x00, 0x00};
    const auto decoded_direct = spark::symbol_guess::linux::decodeStrictThunk(direct_thunk, base);
    CHECK(decoded_direct.has_value());
    const auto direct_result = decoded_direct.value_or(spark::symbol_guess::linux::DecodedThunk{});
    CHECK(direct_result.target == 0x200);
    CHECK(!direct_result.indirect);
    CHECK(!direct_result.adjusts_this);

    constexpr std::array<std::uint8_t, 9> adjustor_thunk = {0x48, 0x83, 0xc7, 0xc8, 0xe9, 0xf7, 0x00, 0x00, 0x00};
    const auto decoded_adjustor = spark::symbol_guess::linux::decodeStrictThunk(adjustor_thunk, base);
    CHECK(decoded_adjustor.has_value());
    const auto adjustor_result = decoded_adjustor.value_or(spark::symbol_guess::linux::DecodedThunk{});
    CHECK(adjustor_result.target == 0x200);
    CHECK(!adjustor_result.indirect);
    CHECK(adjustor_result.adjusts_this);

    constexpr std::array<std::uint8_t, 9> lea_adjustor_thunk = {0x48, 0x8d, 0x7f, 0xc8, 0xe9, 0xf7, 0x00, 0x00, 0x00};
    const auto decoded_lea_adjustor = spark::symbol_guess::linux::decodeStrictThunk(lea_adjustor_thunk, base);
    CHECK(decoded_lea_adjustor.has_value());
    const auto lea_adjustor_result = decoded_lea_adjustor.value_or(spark::symbol_guess::linux::DecodedThunk{});
    CHECK(lea_adjustor_result.target == 0x200);
    CHECK(lea_adjustor_result.adjusts_this);

    constexpr std::array<std::uint8_t, 6> got_thunk = {0xff, 0x25, 0xfa, 0x00, 0x00, 0x00};
    const auto decoded_got = spark::symbol_guess::linux::decodeStrictThunk(got_thunk, base);
    CHECK(decoded_got.has_value());
    const auto got_result = decoded_got.value_or(spark::symbol_guess::linux::DecodedThunk{});
    CHECK(got_result.target == 0x200);
    CHECK(got_result.indirect);
    constexpr std::array<std::uint8_t, 3> object_dispatch = {0xff, 0x67, 0x08};
    CHECK(!spark::symbol_guess::linux::decodeStrictThunk(object_dispatch, base));

    constexpr std::array<std::uint8_t, 9> got_register_thunk = {0x48, 0x8b, 0x05, 0xf9, 0x00, 0x00, 0x00, 0xff, 0xe0};
    const auto decoded_got_register = spark::symbol_guess::linux::decodeStrictThunk(got_register_thunk, base);
    CHECK(decoded_got_register.has_value());
    const auto got_register_result = decoded_got_register.value_or(spark::symbol_guess::linux::DecodedThunk{});
    CHECK(got_register_result.target == 0x200);
    CHECK(got_register_result.indirect);

    // A real wrapper has a side effect before its jump and is not a thunk.
    constexpr std::array<std::uint8_t, 8> side_effect = {0x48, 0xff, 0xc0, 0xe9, 0xf8, 0x00, 0x00, 0x00};
    CHECK(!spark::symbol_guess::linux::decodeStrictThunk(side_effect, base));
    constexpr std::array<std::uint8_t, 11> embedded_jump = {0x48, 0xb8, 0xe9, 0xfb, 0x00, 0x00,
                                                            0x00, 0x90, 0x90, 0x90, 0xc3};
    CHECK(!spark::symbol_guess::linux::decodeStrictThunk(embedded_jump, base));

    const auto edge = [](const std::unordered_map<std::uint64_t, std::uint64_t> &edges) {
        return [&edges](std::uint64_t value) -> std::optional<std::uint64_t> {
            const auto it = edges.find(value);
            return it == edges.end() ? std::nullopt : std::optional<std::uint64_t>{it->second};
        };
    };
    const std::unordered_map<std::uint64_t, std::uint64_t> two_edges = {{0x100, 0x200}, {0x200, 0x300}};
    CHECK(spark::symbol_guess::linux::followStrictThunkChain(0x100, edge(two_edges)) ==
          std::optional<std::uint64_t>{0x300});
    const std::unordered_map<std::uint64_t, std::uint64_t> loop = {{0x100, 0x200}, {0x200, 0x100}};
    CHECK(!spark::symbol_guess::linux::followStrictThunkChain(0x100, edge(loop)));
    const std::unordered_map<std::uint64_t, std::uint64_t> too_deep = {{0x100, 0x200}, {0x200, 0x300}, {0x300, 0x400}};
    CHECK(!spark::symbol_guess::linux::followStrictThunkChain(0x100, edge(too_deep)));

    if (failures != 0) {
        std::cerr << failures << " Linux symbol guess test(s) failed\n";
        return 1;
    }
    std::cout << "Linux symbol guess tests passed\n";
    return 0;
}

#endif
