#include "native/symbol/symbol_guess_dwarf.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace spark::symbol_guess::dwarf {
namespace {

constexpr std::uint8_t kOmit = 0xff;
constexpr std::uint8_t kAbsptr = 0x00;
constexpr std::uint8_t kUleb128 = 0x01;
constexpr std::uint8_t kUdata2 = 0x02;
constexpr std::uint8_t kUdata4 = 0x03;
constexpr std::uint8_t kUdata8 = 0x04;
constexpr std::uint8_t kSleb128 = 0x09;
constexpr std::uint8_t kSdata2 = 0x0a;
constexpr std::uint8_t kSdata4 = 0x0b;
constexpr std::uint8_t kSdata8 = 0x0c;
constexpr std::uint8_t kPcrel = 0x10;
constexpr std::uint8_t kTextrel = 0x20;
constexpr std::uint8_t kDatarel = 0x30;
constexpr std::uint8_t kFuncrel = 0x40;
constexpr std::uint8_t kAligned = 0x50;
constexpr std::uint8_t kIndirect = 0x80;

constexpr std::uint64_t kMaximumEntries = 4u << 20;
constexpr std::uint64_t kMaximumRecordBytes = 1u << 20;
constexpr std::size_t kMaximumAugmentationBytes = 4096;

template <typename T>
bool read(const ImageView &image, std::uint64_t rva, T &out)
{
    return image.read(rva, &out, sizeof(out));
}

bool advance(std::uint64_t &cursor, std::uint64_t amount, std::uint64_t limit)
{
    if (cursor > limit || amount > limit - cursor) {
        return false;
    }
    cursor += amount;
    return true;
}

bool readUleb(const ImageView &image, std::uint64_t &cursor, std::uint64_t limit, std::uint64_t &out)
{
    out = 0;
    unsigned shift = 0;
    for (unsigned i = 0; i < 10 && cursor < limit; ++i) {
        std::uint8_t byte = 0;
        if (!read(image, cursor, byte) || !advance(cursor, 1, limit)) {
            return false;
        }
        if (shift == 63 && (byte & 0x7e) != 0) {
            return false;
        }
        out |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            return true;
        }
        shift += 7;
    }
    return false;
}

bool readSleb(const ImageView &image, std::uint64_t &cursor, std::uint64_t limit, std::int64_t &out)
{
    std::uint64_t value = 0;
    unsigned shift = 0;
    std::uint8_t byte = 0;
    for (unsigned i = 0; i < 10 && cursor < limit; ++i) {
        if (!read(image, cursor, byte) || !advance(cursor, 1, limit)) {
            return false;
        }
        if (shift == 63 && (byte & 0x7e) != 0 && (byte & 0x7f) != 0x7f) {
            return false;
        }
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        shift += 7;
        if ((byte & 0x80) == 0) {
            if (shift < 64 && (byte & 0x40) != 0) {
                value |= (~std::uint64_t{0}) << shift;
            }
            std::memcpy(&out, &value, sizeof(out));
            return true;
        }
    }
    return false;
}

bool addSigned(std::uint64_t base, std::int64_t offset, std::uint64_t &out)
{
    if (offset >= 0) {
        const auto positive = static_cast<std::uint64_t>(offset);
        if (positive > (std::numeric_limits<std::uint64_t>::max)() - base) {
            return false;
        }
        out = base + positive;
        return true;
    }
    const std::uint64_t magnitude = static_cast<std::uint64_t>(-(offset + 1)) + 1;
    if (magnitude > base) {
        return false;
    }
    out = base - magnitude;
    return true;
}

struct EncodedValue {
    std::uint64_t unsigned_value = 0;
    std::int64_t signed_value = 0;
    bool is_signed = false;
};

bool readRawEncoded(const ImageView &image, std::uint8_t format, std::uint8_t address_size, std::uint64_t &cursor,
                    std::uint64_t limit, EncodedValue &out)
{
    const std::uint64_t start = cursor;
    switch (format) {
    case kAbsptr:
        if (address_size == 4) {
            std::uint32_t value = 0;
            if (!read(image, cursor, value) || !advance(cursor, 4, limit)) {
                return false;
            }
            out.unsigned_value = value;
            return true;
        }
        if (address_size == 8) {
            std::uint64_t value = 0;
            if (!read(image, cursor, value) || !advance(cursor, 8, limit)) {
                return false;
            }
            out.unsigned_value = value;
            return true;
        }
        return false;
    case kUleb128:
        return readUleb(image, cursor, limit, out.unsigned_value);
    case kSleb128:
        out.is_signed = true;
        return readSleb(image, cursor, limit, out.signed_value);
    case kUdata2: {
        std::uint16_t value = 0;
        if (!read(image, cursor, value) || !advance(cursor, 2, limit)) {
            return false;
        }
        out.unsigned_value = value;
        return true;
    }
    case kUdata4: {
        std::uint32_t value = 0;
        if (!read(image, cursor, value) || !advance(cursor, 4, limit)) {
            return false;
        }
        out.unsigned_value = value;
        return true;
    }
    case kUdata8: {
        std::uint64_t value = 0;
        if (!read(image, cursor, value) || !advance(cursor, 8, limit)) {
            return false;
        }
        out.unsigned_value = value;
        return true;
    }
    case kSdata2: {
        std::int16_t value = 0;
        if (!read(image, cursor, value) || !advance(cursor, 2, limit)) {
            return false;
        }
        out.is_signed = true;
        out.signed_value = value;
        return true;
    }
    case kSdata4: {
        std::int32_t value = 0;
        if (!read(image, cursor, value) || !advance(cursor, 4, limit)) {
            return false;
        }
        out.is_signed = true;
        out.signed_value = value;
        return true;
    }
    case kSdata8: {
        std::int64_t value = 0;
        if (!read(image, cursor, value) || !advance(cursor, 8, limit)) {
            return false;
        }
        out.is_signed = true;
        out.signed_value = value;
        return true;
    }
    default:
        cursor = start;
        return false;
    }
}

bool readEncoded(const ImageView &image, std::uint8_t encoding, std::uint8_t address_size,
                 std::optional<std::uint64_t> section_base, EncodingBases bases,
                 std::optional<std::uint64_t> function_base, std::uint64_t &cursor, std::uint64_t limit,
                 std::uint64_t &out)
{
    if (encoding == kOmit) {
        return false;
    }
    if ((encoding & 0x70) == kAligned) {
        const std::uint64_t alignment = address_size;
        if (alignment == 0 || cursor > (std::numeric_limits<std::uint64_t>::max)() - (alignment - 1)) {
            return false;
        }
        cursor = (cursor + alignment - 1) & ~(alignment - 1);
    }
    const std::uint64_t field = cursor;
    EncodedValue raw{};
    if (!readRawEncoded(image, encoding & 0x0f, address_size, cursor, limit, raw)) {
        return false;
    }

    std::uint64_t value = 0;
    const std::uint8_t application = encoding & 0x70;
    std::uint64_t base = 0;
    switch (application) {
    case 0:
    case kAligned:
        break;
    case kPcrel:
        base = field;
        break;
    case kTextrel:
        if (!bases.has_text) {
            return false;
        }
        base = bases.text;
        break;
    case kDatarel:
        if (section_base) {
            base = *section_base;
        }
        else if (bases.has_data) {
            base = bases.data;
        }
        else {
            return false;
        }
        break;
    case kFuncrel:
        if (!function_base) {
            return false;
        }
        base = *function_base;
        break;
    default:
        return false;
    }
    if (raw.is_signed) {
        if (!addSigned(base, raw.signed_value, value)) {
            return false;
        }
    }
    else {
        if (raw.unsigned_value > (std::numeric_limits<std::uint64_t>::max)() - base) {
            return false;
        }
        value = base + raw.unsigned_value;
    }
    if ((encoding & kIndirect) != 0) {
        if (address_size == 4) {
            std::uint32_t indirect = 0;
            if (!read(image, value, indirect)) {
                return false;
            }
            value = indirect;
        }
        else if (address_size == 8) {
            if (!read(image, value, value)) {
                return false;
            }
        }
        else {
            return false;
        }
    }
    out = value;
    return true;
}

struct Record {
    std::uint64_t content = 0;
    std::uint64_t end = 0;
    bool dwarf64 = false;
};

std::optional<Record> readRecord(const ImageView &image, std::uint64_t record_rva)
{
    if (record_rva > (std::numeric_limits<std::uint64_t>::max)() - 4) {
        return std::nullopt;
    }
    std::uint32_t length32 = 0;
    if (!read(image, record_rva, length32) || length32 == 0) {
        return std::nullopt;
    }
    std::uint64_t content = record_rva + 4;
    std::uint64_t length = length32;
    bool dwarf64 = false;
    if (length32 == 0xffffffffu) {
        if (!read(image, content, length) || content > (std::numeric_limits<std::uint64_t>::max)() - 8) {
            return std::nullopt;
        }
        content += 8;
        dwarf64 = true;
    }
    if (length == 0 || length > kMaximumRecordBytes || content > (std::numeric_limits<std::uint64_t>::max)() - length) {
        return std::nullopt;
    }
    const std::uint64_t end = content + length;
    std::uint8_t last = 0;
    if (!read(image, end - 1, last)) {
        return std::nullopt;
    }
    return Record{content, end, dwarf64};
}

struct CieInfo {
    std::uint8_t address_size = 8;
    std::uint8_t fde_encoding = kAbsptr;
    bool has_z_augmentation = false;
};

std::optional<CieInfo> parseCie(const ImageView &image, std::uint64_t cie_rva, EncodingBases bases)
{
    const auto record = readRecord(image, cie_rva);
    if (!record) {
        return std::nullopt;
    }
    std::uint64_t cursor = record->content;
    std::uint64_t cie_id = 1;
    if (record->dwarf64) {
        if (!read(image, cursor, cie_id) || !advance(cursor, 8, record->end)) {
            return std::nullopt;
        }
    }
    else {
        std::uint32_t id32 = 1;
        if (!read(image, cursor, id32) || !advance(cursor, 4, record->end)) {
            return std::nullopt;
        }
        cie_id = id32;
    }
    if (cie_id != 0) {
        return std::nullopt;
    }
    std::uint8_t version = 0;
    if (!read(image, cursor, version) || !advance(cursor, 1, record->end) ||
        (version != 1 && version != 3 && version != 4)) {
        return std::nullopt;
    }

    std::string augmentation;
    bool augmentation_terminated = false;
    for (std::size_t i = 0; i <= 64 && cursor < record->end; ++i) {
        std::uint8_t ch = 0;
        if (!read(image, cursor, ch) || !advance(cursor, 1, record->end)) {
            return std::nullopt;
        }
        if (ch == 0) {
            augmentation_terminated = true;
            break;
        }
        if (i == 64 || ch < 0x20 || ch > 0x7e) {
            return std::nullopt;
        }
        augmentation.push_back(static_cast<char>(ch));
    }
    if (!augmentation_terminated) {
        return std::nullopt;
    }

    CieInfo info{};
    if (version == 4) {
        std::uint8_t segment_size = 0;
        if (!read(image, cursor, info.address_size) || !advance(cursor, 1, record->end) ||
            !read(image, cursor, segment_size) || !advance(cursor, 1, record->end) ||
            (info.address_size != 4 && info.address_size != 8) || segment_size != 0) {
            return std::nullopt;
        }
    }
    std::uint64_t code_alignment = 0;
    std::int64_t data_alignment = 0;
    if (!readUleb(image, cursor, record->end, code_alignment) || code_alignment == 0 ||
        !readSleb(image, cursor, record->end, data_alignment)) {
        return std::nullopt;
    }
    std::uint64_t return_register = 0;
    if (version == 1) {
        std::uint8_t value = 0;
        if (!read(image, cursor, value) || !advance(cursor, 1, record->end)) {
            return std::nullopt;
        }
        return_register = value;
    }
    else if (!readUleb(image, cursor, record->end, return_register)) {
        return std::nullopt;
    }
    (void)data_alignment;
    (void)return_register;

    info.has_z_augmentation = !augmentation.empty() && augmentation[0] == 'z';
    if (!info.has_z_augmentation) {
        return augmentation.empty() ? std::optional(info) : std::nullopt;
    }
    std::uint64_t augmentation_length = 0;
    if (!readUleb(image, cursor, record->end, augmentation_length) || augmentation_length > kMaximumAugmentationBytes ||
        augmentation_length > record->end - cursor) {
        return std::nullopt;
    }
    const std::uint64_t augmentation_end = cursor + augmentation_length;
    for (std::size_t i = 1; i < augmentation.size(); ++i) {
        switch (augmentation[i]) {
        case 'L': {
            std::uint8_t ignored = 0;
            if (!read(image, cursor, ignored) || !advance(cursor, 1, augmentation_end)) {
                return std::nullopt;
            }
            break;
        }
        case 'R':
            if (!read(image, cursor, info.fde_encoding) || !advance(cursor, 1, augmentation_end) ||
                info.fde_encoding == kOmit) {
                return std::nullopt;
            }
            break;
        case 'P': {
            std::uint8_t encoding = 0;
            std::uint64_t ignored = 0;
            if (!read(image, cursor, encoding) || !advance(cursor, 1, augmentation_end) ||
                !readEncoded(image, encoding, info.address_size, std::nullopt, bases, std::nullopt, cursor,
                             augmentation_end, ignored)) {
                return std::nullopt;
            }
            break;
        }
        case 'S':
            break;
        default:
            return std::nullopt;
        }
    }
    if (cursor != augmentation_end) {
        return std::nullopt;
    }
    return info;
}

std::optional<FunctionRange> parseFde(const ImageView &image, std::uint64_t fde_rva, std::uint64_t eh_frame_rva,
                                      std::optional<std::uint64_t> expected_initial, EncodingBases bases,
                                      std::unordered_map<std::uint64_t, std::optional<CieInfo>> &cies)
{
    const auto record = readRecord(image, fde_rva);
    if (!record || fde_rva < eh_frame_rva) {
        return std::nullopt;
    }
    std::uint64_t cursor = record->content;
    const std::uint64_t cie_pointer_field = cursor;
    std::uint64_t cie_offset = 0;
    if (record->dwarf64) {
        if (!read(image, cursor, cie_offset) || !advance(cursor, 8, record->end)) {
            return std::nullopt;
        }
    }
    else {
        std::uint32_t offset32 = 0;
        if (!read(image, cursor, offset32) || !advance(cursor, 4, record->end)) {
            return std::nullopt;
        }
        cie_offset = offset32;
    }
    if (cie_offset == 0 || cie_offset > cie_pointer_field) {
        return std::nullopt;
    }
    const std::uint64_t cie_rva = cie_pointer_field - cie_offset;
    if (cie_rva < eh_frame_rva) {
        return std::nullopt;
    }
    auto [it, inserted] = cies.try_emplace(cie_rva);
    if (inserted) {
        it->second = parseCie(image, cie_rva, bases);
    }
    if (!it->second) {
        return std::nullopt;
    }
    const CieInfo &cie = *it->second;
    std::uint64_t initial = 0;
    if (!readEncoded(image, cie.fde_encoding, cie.address_size, std::nullopt, bases, std::nullopt, cursor, record->end,
                     initial) ||
        (expected_initial && initial != *expected_initial)) {
        return std::nullopt;
    }
    EncodedValue raw_range{};
    if (!readRawEncoded(image, cie.fde_encoding & 0x0f, cie.address_size, cursor, record->end, raw_range)) {
        return std::nullopt;
    }
    const std::uint64_t length =
        raw_range.is_signed ? (raw_range.signed_value > 0 ? static_cast<std::uint64_t>(raw_range.signed_value) : 0)
                            : raw_range.unsigned_value;
    if (length == 0 || length > (std::numeric_limits<std::uint64_t>::max)() - initial ||
        length > (std::numeric_limits<std::size_t>::max)() ||
        !image.executable(initial, static_cast<std::size_t>(length))) {
        return std::nullopt;
    }
    if (cie.has_z_augmentation) {
        std::uint64_t augmentation_length = 0;
        if (!readUleb(image, cursor, record->end, augmentation_length) ||
            augmentation_length > kMaximumAugmentationBytes || !advance(cursor, augmentation_length, record->end)) {
            return std::nullopt;
        }
    }
    return FunctionRange{initial, initial + length, initial};
}

}  // namespace

std::vector<FunctionRange> parseEhFrameHeader(const ImageView &image, std::uint64_t header_rva,
                                              std::uint64_t header_size, EncodingBases bases, ParseStats *stats)
{
    ParseStats local{};
    auto finish = [&](std::vector<FunctionRange> ranges) {
        local.function_ranges = ranges.size();
        if (stats != nullptr) {
            *stats = local;
        }
        return ranges;
    };
    if (header_size < 4 || header_rva > (std::numeric_limits<std::uint64_t>::max)() - header_size) {
        return finish({});
    }
    const std::uint64_t limit = header_rva + header_size;
    std::array<std::uint8_t, 4> header{};
    if (!image.read(header_rva, header.data(), header.size()) || header[0] != 1 || header[1] == kOmit ||
        header[2] == kOmit || header[3] == kOmit) {
        return finish({});
    }
    std::uint64_t cursor = header_rva + 4;
    std::uint64_t eh_frame = 0;
    if (!readEncoded(image, header[1], 8, header_rva, bases, std::nullopt, cursor, limit, eh_frame)) {
        return finish({});
    }
    std::uint64_t count = 0;
    if (!readEncoded(image, header[2], 8, header_rva, bases, std::nullopt, cursor, limit, count) || count == 0 ||
        count > kMaximumEntries) {
        return finish({});
    }
    local.table_entries = static_cast<std::size_t>(count);
    std::uint8_t eh_probe = 0;
    if (!read(image, eh_frame, eh_probe)) {
        return finish({});
    }

    std::vector<FunctionRange> candidates;
    candidates.reserve(static_cast<std::size_t>(count));
    std::unordered_map<std::uint64_t, std::optional<CieInfo>> cies;
    std::unordered_set<std::uint64_t> indexed_fdes;
    indexed_fdes.reserve(static_cast<std::size_t>(count));
    std::uint64_t previous_initial = 0;
    bool have_previous_initial = false;
    bool monotonic = true;
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint64_t initial = 0;
        std::uint64_t fde = 0;
        if (!readEncoded(image, header[3], 8, header_rva, bases, std::nullopt, cursor, limit, initial) ||
            !readEncoded(image, header[3], 8, header_rva, bases, std::nullopt, cursor, limit, fde)) {
            local.rejected_entries += static_cast<std::size_t>(count - i);
            break;
        }
        if (have_previous_initial && initial < previous_initial) {
            monotonic = false;
        }
        previous_initial = initial;
        have_previous_initial = true;
        indexed_fdes.insert(fde);
        const auto range = parseFde(image, fde, eh_frame, initial, bases, cies);
        if (!range) {
            ++local.rejected_entries;
            continue;
        }
        candidates.push_back(*range);
    }
    if (!monotonic) {
        local.rejected_entries = local.table_entries;
        return finish({});
    }

    // A linker may omit a valid FDE from the search table; walk .eh_frame records
    // within the containing PT_LOAD as a fallback, accepting unindexed entries only
    // when the walk reaches the zero terminator.
    const std::uint64_t eh_frame_end = image.readableEnd(eh_frame);
    if (eh_frame_end > eh_frame) {
        std::vector<FunctionRange> recovered;
        std::uint64_t record_rva = eh_frame;
        bool terminated = false;
        for (std::uint64_t record_count = 0; record_count <= kMaximumEntries && record_rva < eh_frame_end;
             ++record_count) {
            std::uint32_t length32 = 0;
            if (!read(image, record_rva, length32)) {
                break;
            }
            if (length32 == 0) {
                terminated = true;
                break;
            }
            const auto record = readRecord(image, record_rva);
            if (!record || record->end > eh_frame_end || record->end <= record_rva) {
                break;
            }
            ++local.eh_frame_records;
            std::uint64_t id = 0;
            if (record->dwarf64) {
                if (!read(image, record->content, id)) {
                    break;
                }
            }
            else {
                std::uint32_t id32 = 0;
                if (!read(image, record->content, id32)) {
                    break;
                }
                id = id32;
            }
            if (id != 0 && !indexed_fdes.contains(record_rva)) {
                if (auto range = parseFde(image, record_rva, eh_frame, std::nullopt, bases, cies)) {
                    recovered.push_back(*range);
                }
            }
            record_rva = record->end;
        }
        if (terminated) {
            local.unindexed_ranges = recovered.size();
            candidates.insert(candidates.end(), recovered.begin(), recovered.end());
        }
        else {
            local.eh_frame_records = 0;
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const FunctionRange &a, const FunctionRange &b) {
        return a.begin != b.begin ? a.begin < b.begin : a.end < b.end;
    });
    std::vector<FunctionRange> unique;
    unique.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size();) {
        std::size_t next = i + 1;
        while (next < candidates.size() && candidates[next].begin == candidates[i].begin) {
            ++next;
        }
        bool conflict = false;
        for (std::size_t j = i + 1; j < next; ++j) {
            if (candidates[j].end != candidates[i].end) {
                conflict = true;
            }
        }
        if (conflict) {
            local.rejected_entries += next - i;
        }
        else {
            unique.push_back(candidates[i]);
            local.duplicate_ranges += next - i - 1;
        }
        i = next;
    }

    std::vector<bool> overlapping(unique.size(), false);
    for (std::size_t group = 0; group < unique.size();) {
        std::size_t next = group + 1;
        std::uint64_t group_end = unique[group].end;
        while (next < unique.size() && unique[next].begin < group_end) {
            group_end = std::max(group_end, unique[next].end);
            ++next;
        }
        if (next - group > 1) {
            std::fill(overlapping.begin() + static_cast<std::ptrdiff_t>(group),
                      overlapping.begin() + static_cast<std::ptrdiff_t>(next), true);
        }
        group = next;
    }
    std::vector<FunctionRange> ranges;
    ranges.reserve(unique.size());
    for (std::size_t i = 0; i < unique.size(); ++i) {
        if (overlapping[i]) {
            ++local.overlap_ranges;
            ++local.rejected_entries;
        }
        else {
            ranges.push_back(unique[i]);
        }
    }
    for (std::size_t i = 1; i < ranges.size(); ++i) {
        if (ranges[i - 1].end < ranges[i].begin) {
            ++local.gap_ranges;
            local.gap_bytes += ranges[i].begin - ranges[i - 1].end;
        }
    }
    return finish(std::move(ranges));
}

const FunctionRange *functionContaining(const std::vector<FunctionRange> &ranges, std::uint64_t rva)
{
    auto it = std::upper_bound(ranges.begin(), ranges.end(), rva,
                               [](std::uint64_t value, const FunctionRange &r) { return value < r.begin; });
    if (it == ranges.begin()) {
        return nullptr;
    }
    --it;
    return rva < it->end ? &*it : nullptr;
}

}  // namespace spark::symbol_guess::dwarf
