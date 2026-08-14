#include <cassert>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/profiler/profiler.h"

namespace spark {

struct ProfilerTestAccess {
    static void addNativePluginSources(ProfileMetadata &meta, const ExportContext &ctx,
                                       const std::vector<FrameKey> &keys,
                                       const std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> &resolved)
    {
        Profiler::addNativePluginSources(meta, ctx, keys, resolved);
    }
};

}  // namespace spark

namespace {

std::uint64_t readVarint(const std::string &data, std::size_t &offset)
{
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        assert(offset < data.size());
        const auto byte = static_cast<std::uint8_t>(data[offset++]);
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            return value;
        }
    }
    assert(false);
    return 0;
}

std::string readBytes(const std::string &data, std::size_t &offset)
{
    const auto size = static_cast<std::size_t>(readVarint(data, offset));
    assert(size <= data.size() - offset);
    std::string value = data.substr(offset, size);
    offset += size;
    return value;
}

void skipField(const std::string &data, std::size_t &offset, std::uint64_t wire_type)
{
    if (wire_type == 0) {
        static_cast<void>(readVarint(data, offset));
    }
    else if (wire_type == 1) {
        assert(8 <= data.size() - offset);
        offset += 8;
    }
    else if (wire_type == 2) {
        static_cast<void>(readBytes(data, offset));
    }
    else if (wire_type == 5) {
        assert(4 <= data.size() - offset);
        offset += 4;
    }
    else {
        assert(false);
    }
}

std::map<std::string, std::string> classSources(const std::string &payload)
{
    std::map<std::string, std::string> result;
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const std::uint64_t tag = readVarint(payload, offset);
        if ((tag >> 3) != 3) {
            skipField(payload, offset, tag & 7);
            continue;
        }

        const std::string entry = readBytes(payload, offset);
        std::size_t entry_offset = 0;
        std::string class_name;
        std::string source_id;
        while (entry_offset < entry.size()) {
            const std::uint64_t entry_tag = readVarint(entry, entry_offset);
            if ((entry_tag >> 3) == 1) {
                class_name = readBytes(entry, entry_offset);
            }
            else if ((entry_tag >> 3) == 2) {
                source_id = readBytes(entry, entry_offset);
            }
            else {
                skipField(entry, entry_offset, entry_tag & 7);
            }
        }
        result.emplace(std::move(class_name), std::move(source_id));
    }
    return result;
}

spark::FrameKey frame(std::uint32_t module, std::uintptr_t base, std::uint64_t rva)
{
    return {.module = module, .rva = rva, .raw_address = base + rva};
}

}  // namespace

int main()
{
    spark::ExportContext context;
    context.native_plugin_sources = {
        {.module_base = 0x100000, .module_path = "plugin-a", .source_id = "plugin_a"},
        {.module_base = 0x200000, .module_path = "plugin-b", .source_id = "plugin_b"},
    };

    const spark::FrameKey plugin_a = frame(1, 0x100000, 0x120);
    const spark::FrameKey plugin_a_duplicate = frame(1, 0x100000, 0x220);
    const spark::FrameKey unrelated = frame(2, 0x300000, 0x120);
    std::unordered_map<spark::FrameKey, spark::ResolvedFrame, spark::FrameKeyHash> resolved = {
        {plugin_a, {.class_name = "plugin-a.dll", .method_name = "run"}},
        {plugin_a_duplicate, {.class_name = "plugin-a.dll", .method_name = "tick"}},
        {unrelated, {.class_name = "bedrock_server", .method_name = "tick"}},
    };

    spark::ProfileMetadata metadata;
    spark::ProfilerTestAccess::addNativePluginSources(metadata, context, {plugin_a, plugin_a_duplicate, unrelated},
                                                      resolved);
    const std::map<std::string, std::string> expected_sources{{"plugin-a.dll", "plugin_a"}};
    assert(metadata.class_sources == expected_sources);

    const std::string payload = spark::buildSamplerData(metadata, spark::CallTree{}, resolved);
    assert(classSources(payload) == metadata.class_sources);

    const spark::FrameKey conflicting = frame(3, 0x200000, 0x320);
    resolved.emplace(conflicting, spark::ResolvedFrame{.class_name = "plugin-a.dll", .method_name = "conflict"});
    spark::ProfilerTestAccess::addNativePluginSources(metadata, context, {conflicting}, resolved);
    assert(metadata.class_sources.empty());

    const spark::FrameKey underflow{.module = 4, .rva = 0x200, .raw_address = 0x100};
    resolved.emplace(underflow, spark::ResolvedFrame{.class_name = "invalid", .method_name = "invalid"});
    spark::ProfilerTestAccess::addNativePluginSources(metadata, context, {underflow}, resolved);
    assert(metadata.class_sources.empty());
}
