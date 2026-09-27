#ifndef ENDSTONE_SPARK_LINUX_ELF_ADMISSION_H
#define ENDSTONE_SPARK_LINUX_ELF_ADMISSION_H

#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "native/alloc/linux_gateway_identity.h"

namespace spark::gateway::elf {

constexpr std::size_t KImages = 4096;
constexpr std::size_t KEdges = 16384;
constexpr std::size_t KSymbols = 1048576;
constexpr std::size_t KVersions = 65536;
constexpr std::size_t KRelocations = 262144;
constexpr std::size_t KMetadata = 64 * 1024 * 1024;
constexpr std::int64_t KDtRelrSz = 35;

inline void require(bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

inline std::uint64_t add(std::uint64_t first, std::uint64_t second)
{
    require(second <= std::numeric_limits<std::uint64_t>::max() - first, "ELF address overflow");
    return first + second;
}

inline std::uint64_t signedAdd(std::uint64_t value, std::int64_t offset)
{
    if (offset >= 0) {
        return add(value, static_cast<std::uint64_t>(offset));
    }
    const auto magnitude = static_cast<std::uint64_t>(-(offset + 1)) + 1;
    require(value >= magnitude, "ELF relocation underflow");
    return value - magnitude;
}

inline std::uint32_t sysvHash(std::string_view name) noexcept
{
    std::uint32_t hash = 0;
    for (const auto character : name) {
        hash = (hash << 4) + static_cast<unsigned char>(character);
        const auto high = hash & 0xf0000000U;
        hash ^= high >> 24;
        hash &= ~high;
    }
    return hash;
}

inline std::uint32_t gnuHash(std::string_view name) noexcept
{
    std::uint32_t hash = 5381;
    for (const auto character : name) {
        hash = hash * 33 + static_cast<unsigned char>(character);
    }
    return hash;
}

class File {
public:
    explicit File(const std::string &path) : descriptor_(::open(path.c_str(), O_RDONLY | O_CLOEXEC))
    {
        require(descriptor_ >= 0, "cannot open ELF metadata");
    }
    ~File() { ::close(descriptor_); }
    File(const File &) = delete;
    File &operator=(const File &) = delete;

    void read(std::uint64_t offset, void *destination, std::size_t size)
    {
        require(size <= KMetadata - bytes_ && offset <= std::numeric_limits<off_t>::max() &&
                    size <= static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - offset,
                "ELF metadata exceeds bounds");
        bytes_ += size;
        require(::pread(descriptor_, destination, size, static_cast<off_t>(offset)) == static_cast<ssize_t>(size),
                "cannot read complete ELF metadata");
    }
    template <typename T>
    T read(std::uint64_t offset)
    {
        T value{};
        read(offset, &value, sizeof(value));
        return value;
    }
    int descriptor() const noexcept { return descriptor_; }

private:
    int descriptor_;
    std::size_t bytes_ = 0;
};

struct Version {
    std::string name;
    std::uint32_t hash = 0;
    bool definition = false;
};

struct Object {
    struct Range {
        std::uintptr_t begin;
        std::uintptr_t end;
        unsigned flags;
    };
    Identity identity;
    std::uintptr_t bias = 0;
    std::string loader_name;
    std::vector<Elf64_Phdr> headers;
    std::vector<Range> mapped_ranges;
    std::vector<Elf64_Dyn> dynamic;
    std::vector<char> strings;
    std::vector<Elf64_Sym> symbols;
    std::vector<bool> lookup_symbols;
    std::vector<Elf64_Half> versions;
    std::map<std::uint16_t, Version> version_names;
    std::vector<std::string> needed;
    std::string soname;
    std::vector<Elf64_Rela> relocations;
    std::uint16_t type = 0;

    std::uint64_t tag(std::int64_t wanted) const
    {
        std::uint64_t result = 0;
        bool found = false;
        for (const auto &entry : dynamic) {
            if (entry.d_tag == wanted) {
                require(!found, "duplicate ELF dynamic tag");
                result = entry.d_un.d_val;
                found = true;
            }
        }
        return result;
    }
    std::string string(std::uint64_t offset) const
    {
        require(offset < strings.size(), "ELF string offset outside table");
        const auto *begin = strings.data() + offset;
        const auto *end = static_cast<const char *>(std::memchr(begin, 0, strings.size() - offset));
        require(end != nullptr, "unterminated ELF string");
        return {begin, end};
    }
    bool contains(std::uint64_t address, std::uint64_t size, unsigned flags, bool loaded = true) const
    {
        for (const auto &header : headers) {
            if (header.p_type != PT_LOAD || (header.p_flags & flags) != flags) {
                continue;
            }
            const auto begin = add(loaded ? bias : 0, header.p_vaddr);
            if (address >= begin && address - begin <= header.p_memsz && size <= header.p_memsz - (address - begin)) {
                if (!loaded) {
                    return true;
                }
                return std::any_of(mapped_ranges.begin(), mapped_ranges.end(), [&](const Range &range) {
                    return address >= range.begin && address < range.end && size <= range.end - address &&
                           (range.flags & flags) == flags;
                });
            }
        }
        return false;
    }
    std::uint64_t offset(std::uint64_t address, std::size_t size) const
    {
        for (const auto &header : headers) {
            if (header.p_type == PT_LOAD && (header.p_flags & PF_R) != 0 && address >= header.p_vaddr &&
                address - header.p_vaddr <= header.p_filesz && size <= header.p_filesz - (address - header.p_vaddr)) {
                return add(header.p_offset, address - header.p_vaddr);
            }
        }
        throw std::runtime_error("ELF metadata outside readable file load segment");
    }
    template <typename T>
    T at(File &file, std::uint64_t address) const
    {
        return file.read<T>(offset(address, sizeof(T)));
    }
    template <typename T>
    std::vector<T> table(File &file, std::uint64_t address, std::size_t count) const
    {
        require(count <= KMetadata / sizeof(T), "ELF table exceeds bounds");
        std::vector<T> result(count);
        if (count != 0) {
            file.read(offset(address, count * sizeof(T)), result.data(), count * sizeof(T));
        }
        return result;
    }
    std::size_t symbolCount(File &file) const
    {
        std::size_t sysv_count = 0;
        if (const auto address = tag(DT_HASH)) {
            const auto buckets = at<std::uint32_t>(file, address);
            sysv_count = at<std::uint32_t>(file, add(address, 4));
            require(buckets != 0 && buckets <= KSymbols && sysv_count != 0 && sysv_count <= KSymbols,
                    "invalid ELF SysV hash bounds");
            const auto values = table<std::uint32_t>(file, add(address, 8), buckets + sysv_count);
            for (const auto value : values) {
                require(value < sysv_count, "ELF SysV hash index outside symbols");
            }
            std::size_t walks = 0;
            for (std::size_t bucket = 0; bucket < buckets; ++bucket) {
                for (auto index = values[bucket]; index != 0; index = values[buckets + index]) {
                    require(++walks <= KSymbols, "ELF SysV hash walk exceeds bounds");
                }
            }
        }
        std::size_t gnu_count = 0;
        if (const auto address = tag(DT_GNU_HASH)) {
            const auto header = table<std::uint32_t>(file, address, 4);
            require(header[0] != 0 && header[0] <= KSymbols && header[1] <= KSymbols && header[2] != 0 &&
                        header[2] <= KSymbols && (header[2] & (header[2] - 1)) == 0 && header[3] < 32,
                    "invalid ELF GNU hash bounds");
            table<Elf64_Xword>(file, add(address, 16), header[2]);
            const auto buckets_address = add(add(address, 16), static_cast<std::uint64_t>(header[2]) * 8);
            const auto buckets = table<std::uint32_t>(file, buckets_address, header[0]);
            const auto chains = add(buckets_address, static_cast<std::uint64_t>(header[0]) * 4);
            gnu_count = header[1];
            std::size_t walks = 0;
            for (auto index : buckets) {
                if (index == 0) {
                    continue;
                }
                require(index >= header[1], "ELF GNU hash bucket precedes symbols");
                for (;;) {
                    require(index < KSymbols && ++walks <= KSymbols, "ELF GNU hash walk exceeds bounds");
                    const auto hash = at<std::uint32_t>(file, add(chains, (index - header[1]) * 4ULL));
                    gnu_count = std::max(gnu_count, static_cast<std::size_t>(++index));
                    if ((hash & 1) != 0) {
                        break;
                    }
                }
            }
        }
        require(sysv_count != 0 || gnu_count != 0, "ELF has no bounded symbol hash");
        require(sysv_count == 0 || gnu_count == 0 || sysv_count == gnu_count, "ELF symbol hash counts disagree");
        return std::max(sysv_count, gnu_count);
    }

    void validateHashes(File &file)
    {
        lookup_symbols.assign(symbols.size(), false);
        std::size_t walks = 0;
        if (const auto address = tag(DT_HASH)) {
            const auto header = table<std::uint32_t>(file, address, 2);
            const auto buckets = header[0];
            require(header[1] == symbols.size(), "ELF SysV symbol count changed");
            const auto values = table<std::uint32_t>(file, add(address, 8), buckets + symbols.size());
            for (std::size_t bucket = 0; bucket < buckets; ++bucket) {
                for (auto index = values[bucket]; index != 0; index = values[buckets + index]) {
                    require(index < symbols.size() && ++walks <= KSymbols && !lookup_symbols[index],
                            "invalid ELF SysV hash chain");
                    require(sysvHash(string(symbols[index].st_name)) % buckets == bucket,
                            "ELF SysV hash bucket disagrees with symbol name");
                    lookup_symbols[index] = true;
                }
            }
        }
        if (const auto address = tag(DT_GNU_HASH)) {
            lookup_symbols.assign(symbols.size(), false);
            const auto header = table<std::uint32_t>(file, address, 4);
            const auto bloom = table<Elf64_Xword>(file, add(address, 16), header[2]);
            const auto buckets_address = add(add(address, 16), static_cast<std::uint64_t>(header[2]) * 8);
            const auto buckets = table<std::uint32_t>(file, buckets_address, header[0]);
            require(header[1] <= symbols.size(), "ELF GNU symbol offset outside table");
            const auto chains =
                table<std::uint32_t>(file, add(buckets_address, header[0] * 4ULL), symbols.size() - header[1]);
            for (std::size_t bucket = 0; bucket < buckets.size(); ++bucket) {
                auto index = buckets[bucket];
                if (index == 0) {
                    continue;
                }
                for (;;) {
                    require(index >= header[1] && index < symbols.size() && ++walks <= KSymbols &&
                                !lookup_symbols[index],
                            "invalid ELF GNU hash chain");
                    const auto expected = gnuHash(string(symbols[index].st_name));
                    const auto actual = chains[index - header[1]];
                    const auto mask = (Elf64_Xword{1} << (expected % 64)) |
                                      (Elf64_Xword{1} << ((static_cast<std::uint64_t>(expected) >> header[3]) % 64));
                    require(expected % buckets.size() == bucket && (actual & ~1U) == (expected & ~1U),
                            "ELF GNU hash disagrees with symbol name");
                    require((bloom[(expected / 64) & (bloom.size() - 1)] & mask) == mask,
                            "ELF GNU bloom filter excludes a bucket symbol");
                    lookup_symbols[index] = true;
                    ++index;
                    if ((actual & 1) != 0) {
                        break;
                    }
                }
            }
        }
    }
    void readVersions(File &file)
    {
        const auto version_address = tag(DT_VERSYM);
        if (version_address != 0) {
            versions = table<Elf64_Half>(file, version_address, symbols.size());
        }
        else {
            versions.resize(symbols.size(), VER_NDX_GLOBAL);
        }
        std::size_t walks = 0;
        auto address = tag(DT_VERDEF);
        const auto definitions = tag(DT_VERDEFNUM);
        require(definitions <= KVersions && ((address == 0) == (definitions == 0)), "invalid ELF version definitions");
        for (std::size_t i = 0; i < definitions; ++i) {
            require(++walks <= KVersions, "ELF version walk exceeds bounds");
            const auto entry = at<Elf64_Verdef>(file, address);
            require(entry.vd_version == VER_DEF_CURRENT && entry.vd_cnt != 0 && entry.vd_aux >= sizeof(entry),
                    "invalid ELF version definition");
            auto auxiliary_address = add(address, entry.vd_aux);
            std::string name;
            for (std::size_t j = 0; j < entry.vd_cnt; ++j) {
                require(++walks <= KVersions, "ELF version walk exceeds bounds");
                const auto auxiliary = at<Elf64_Verdaux>(file, auxiliary_address);
                const auto value = string(auxiliary.vda_name);
                if (j == 0) {
                    name = value;
                }
                require((j + 1 == entry.vd_cnt) ? auxiliary.vda_next == 0 : auxiliary.vda_next >= sizeof(auxiliary),
                        "invalid ELF version auxiliary chain");
                auxiliary_address = add(auxiliary_address, auxiliary.vda_next);
            }
            require(entry.vd_ndx != 0 && !version_names.contains(entry.vd_ndx), "duplicate ELF version index");
            require(entry.vd_hash == sysvHash(name), "ELF version definition hash disagrees with name");
            version_names.emplace(entry.vd_ndx, Version{std::move(name), entry.vd_hash, true});
            require((i + 1 == definitions) ? entry.vd_next == 0 : entry.vd_next >= sizeof(entry),
                    "invalid ELF version definition chain");
            address = add(address, entry.vd_next);
        }
        address = tag(DT_VERNEED);
        const auto needs = tag(DT_VERNEEDNUM);
        require(needs <= KVersions && ((address == 0) == (needs == 0)), "invalid ELF version requirements");
        for (std::size_t i = 0; i < needs; ++i) {
            require(++walks <= KVersions, "ELF version walk exceeds bounds");
            const auto entry = at<Elf64_Verneed>(file, address);
            require(entry.vn_version == VER_NEED_CURRENT && entry.vn_cnt != 0 && entry.vn_aux >= sizeof(entry),
                    "invalid ELF version requirement");
            const auto library = string(entry.vn_file);
            require(std::find(needed.begin(), needed.end(), library) != needed.end(),
                    "ELF version requirement lacks dependency");
            auto auxiliary_address = add(address, entry.vn_aux);
            for (std::size_t j = 0; j < entry.vn_cnt; ++j) {
                require(++walks <= KVersions, "ELF version walk exceeds bounds");
                const auto auxiliary = at<Elf64_Vernaux>(file, auxiliary_address);
                const auto index = auxiliary.vna_other & 0x7fff;
                require(index > VER_NDX_GLOBAL && !version_names.contains(index), "duplicate ELF required version");
                const auto name = string(auxiliary.vna_name);
                require(auxiliary.vna_hash == sysvHash(name), "ELF version requirement hash disagrees with name");
                version_names.emplace(index, Version{name, auxiliary.vna_hash, false});
                require((j + 1 == entry.vn_cnt) ? auxiliary.vna_next == 0 : auxiliary.vna_next >= sizeof(auxiliary),
                        "invalid ELF required version chain");
                auxiliary_address = add(auxiliary_address, auxiliary.vna_next);
            }
            require((i + 1 == needs) ? entry.vn_next == 0 : entry.vn_next >= sizeof(entry),
                    "invalid ELF version requirement chain");
            address = add(address, entry.vn_next);
        }
        for (std::size_t i = 0; i < versions.size(); ++i) {
            const auto index = versions[i] & 0x7fff;
            if (index > VER_NDX_GLOBAL) {
                const auto found = version_names.find(index);
                require(found != version_names.end(), "ELF symbol references missing version");
            }
        }
    }
    void read(const std::string &path, bool helper)
    {
        File file(path);
        const auto header = file.read<Elf64_Ehdr>(0);
        require(std::memcmp(header.e_ident, ELFMAG, SELFMAG) == 0 && header.e_ident[EI_CLASS] == ELFCLASS64 &&
                    header.e_ident[EI_DATA] == ELFDATA2LSB && header.e_ident[EI_VERSION] == EV_CURRENT &&
                    header.e_version == EV_CURRENT && header.e_machine == EM_X86_64 &&
                    (header.e_type == ET_DYN || (!helper && header.e_type == ET_EXEC)) &&
                    header.e_ehsize == sizeof(header) && header.e_phentsize == sizeof(Elf64_Phdr) &&
                    header.e_phnum != 0 && header.e_phnum <= 4096,
                "unsupported ELF header");
        type = header.e_type;
        std::vector<Elf64_Phdr> parsed(header.e_phnum);
        file.read(header.e_phoff, parsed.data(), parsed.size() * sizeof(Elf64_Phdr));
        require(headers.empty() ||
                    (headers.size() == parsed.size() &&
                     std::memcmp(headers.data(), parsed.data(), headers.size() * sizeof(Elf64_Phdr)) == 0),
                "loaded ELF headers differ from file");
        headers = std::move(parsed);
        std::size_t dynamics = 0;
        for (const auto &segment : headers) {
            add(segment.p_vaddr, segment.p_memsz);
            add(segment.p_offset, segment.p_filesz);
            if (segment.p_type == PT_LOAD) {
                require(segment.p_filesz <= segment.p_memsz, "invalid ELF file load segment");
            }
            if (segment.p_type == PT_DYNAMIC) {
                require(++dynamics == 1 && segment.p_filesz % sizeof(Elf64_Dyn) == 0, "invalid ELF dynamic segment");
                dynamic = table<Elf64_Dyn>(file, segment.p_vaddr, segment.p_filesz / sizeof(Elf64_Dyn));
            }
            if (helper) {
                require(segment.p_type != PT_TLS && !(segment.p_type == PT_LOAD && (segment.p_flags & 3) == 3),
                        "helper TLS or executable writable segment");
            }
        }
        require(dynamics == 1, "missing ELF dynamic segment");
        const auto end =
            std::find_if(dynamic.begin(), dynamic.end(), [](const auto &entry) { return entry.d_tag == DT_NULL; });
        require(end != dynamic.end(), "unterminated ELF dynamic segment");
        dynamic.erase(end, dynamic.end());
        std::set<Elf64_Sxword> tags;
        for (const auto &entry : dynamic) {
            require(entry.d_tag == DT_NEEDED || tags.insert(entry.d_tag).second, "duplicate ELF dynamic tag");
        }
        const auto string_size = tag(DT_STRSZ);
        require(string_size != 0 && string_size <= KMetadata && tag(DT_STRTAB) != 0, "invalid ELF string table");
        strings = table<char>(file, tag(DT_STRTAB), string_size);
        require(strings.front() == 0 && strings.back() == 0, "invalid ELF string table terminators");
        for (const auto &entry : dynamic) {
            if (entry.d_tag == DT_NEEDED) {
                require(needed.size() < KEdges, "ELF dependency count exceeds bounds");
                needed.push_back(string(entry.d_un.d_val));
            }
        }
        if (tag(DT_SONAME) != 0) {
            soname = string(tag(DT_SONAME));
        }
        require(tag(DT_SYMENT) == sizeof(Elf64_Sym) && tag(DT_SYMTAB) != 0, "invalid ELF symbol table");
        symbols = table<Elf64_Sym>(file, tag(DT_SYMTAB), symbolCount(file));
        for (const auto &symbol : symbols) {
            string(symbol.st_name);
        }
        validateHashes(file);
        readVersions(file);
        struct stat status{};
        require(::fstat(file.descriptor(), &status) == 0, "cannot identify ELF metadata file");
        if (identity.inode != 0) {
            require(status.st_dev == identity.device && status.st_ino == identity.inode, "ELF file identity changed");
        }
        else {
            identity.device = status.st_dev;
            identity.inode = status.st_ino;
        }
        for (const auto forbidden : {DT_FILTER, DT_AUXILIARY, DT_AUDIT, DT_DEPAUDIT}) {
            require(!tags.contains(forbidden), "ELF dependency has unsupported loader behavior");
        }
        if (!helper) {
            return;
        }
        for (const auto forbidden :
             {DT_INIT, DT_FINI, DT_INIT_ARRAY, DT_FINI_ARRAY, DT_PREINIT_ARRAY, DT_TEXTREL, DT_SYMBOLIC}) {
            require(!tags.contains(forbidden), "helper has unsupported loader behavior");
        }
        require(
            (tag(DT_FLAGS) & ~static_cast<Elf64_Xword>(DF_BIND_NOW)) == 0 &&
                (tag(DT_FLAGS_1) & ~static_cast<Elf64_Xword>(DF_1_NOW)) == 0 &&
                (tags.contains(DT_BIND_NOW) || (tag(DT_FLAGS) & DF_BIND_NOW) != 0 || (tag(DT_FLAGS_1) & DF_1_NOW) != 0),
            "helper requires eager ordinary bindings");
        require(tag(DT_RELSZ) == 0 && tag(KDtRelrSz) == 0, "helper has unsupported REL or RELR relocations");
        require(tag(DT_RELASZ) % sizeof(Elf64_Rela) == 0 && tag(DT_RELAENT) == sizeof(Elf64_Rela),
                "invalid helper RELA table");
        const auto count = tag(DT_RELASZ) / sizeof(Elf64_Rela);
        const auto jump_count = tag(DT_PLTRELSZ) / sizeof(Elf64_Rela);
        require(count <= KRelocations && jump_count <= KRelocations - count &&
                    tag(DT_PLTRELSZ) % sizeof(Elf64_Rela) == 0 && (jump_count == 0 || tag(DT_PLTREL) == DT_RELA),
                "invalid helper relocation bounds");
        relocations = table<Elf64_Rela>(file, tag(DT_RELA), count);
        const auto jumps = table<Elf64_Rela>(file, tag(DT_JMPREL), jump_count);
        relocations.insert(relocations.end(), jumps.begin(), jumps.end());
        require(std::any_of(headers.begin(), headers.end(),
                            [](const auto &segment) { return segment.p_type == PT_GNU_RELRO && segment.p_memsz != 0; }),
                "helper lacks RELRO");
        for (const auto &symbol : symbols) {
            require(ELF64_ST_BIND(symbol.st_info) != STB_GNU_UNIQUE && ELF64_ST_TYPE(symbol.st_info) != STT_GNU_IFUNC &&
                        symbol.st_shndx != SHN_ABS && symbol.st_shndx != SHN_COMMON,
                    "helper has unsupported symbol definition");
        }
        for (const auto &relocation : relocations) {
            const auto kind = ELF64_R_TYPE(relocation.r_info);
            const auto symbol = ELF64_R_SYM(relocation.r_info);
            require(kind == R_X86_64_NONE || kind == R_X86_64_RELATIVE || kind == R_X86_64_64 ||
                        kind == R_X86_64_GLOB_DAT || kind == R_X86_64_JUMP_SLOT,
                    "helper has unsupported relocation");
            require(kind == R_X86_64_NONE || contains(relocation.r_offset, 8, PF_R | PF_W, false),
                    "helper relocation destination outside writable load segment");
            require((kind != R_X86_64_GLOB_DAT && kind != R_X86_64_JUMP_SLOT) ||
                        std::any_of(headers.begin(), headers.end(),
                                    [&](const auto &segment) {
                                        return segment.p_type == PT_GNU_RELRO &&
                                               relocation.r_offset >= segment.p_vaddr &&
                                               relocation.r_offset - segment.p_vaddr <= segment.p_memsz &&
                                               8 <= segment.p_memsz - (relocation.r_offset - segment.p_vaddr);
                                    }),
                    "helper relocation destination lacks RELRO");
            require(symbol < symbols.size() && (kind != R_X86_64_RELATIVE || symbol == 0),
                    "invalid helper relocation symbol");
        }
    }
};

struct Snapshot {
    std::vector<Object> objects;
    std::uint64_t adds = 0;
    std::uint64_t subs = 0;
    bool failed = false;

    static int collect(dl_phdr_info *info, std::size_t size, void *opaque) noexcept
    {
        auto &snapshot = *static_cast<Snapshot *>(opaque);
        try {
            require(size >= offsetof(dl_phdr_info, dlpi_subs) + sizeof(info->dlpi_subs) &&
                        snapshot.objects.size() < KImages && info->dlpi_phnum <= 4096,
                    "loaded ELF snapshot exceeds bounds");
            if (snapshot.objects.empty()) {
                snapshot.adds = info->dlpi_adds;
                snapshot.subs = info->dlpi_subs;
            }
            require(snapshot.adds == info->dlpi_adds && snapshot.subs == info->dlpi_subs,
                    "loader changed during snapshot");
            Object object;
            object.bias = info->dlpi_addr;
            object.loader_name = info->dlpi_name == nullptr ? "" : info->dlpi_name;
            object.headers.assign(info->dlpi_phdr, info->dlpi_phdr + info->dlpi_phnum);
            snapshot.objects.push_back(std::move(object));
            return 0;
        }
        catch (...) {
            snapshot.failed = true;
            return 1;
        }
    }
    void capture(bool metadata = true)
    {
        ::dl_iterate_phdr(collect, this);
        require(!failed && !objects.empty(), "cannot capture loaded ELF identities");
        if (!metadata) {
            return;
        }
        for (auto &object : objects) {
            if (object.loader_name == "linux-vdso.so.1") {
                continue;
            }
            const auto executable = std::find_if(object.headers.begin(), object.headers.end(), [](const auto &header) {
                return header.p_type == PT_LOAD && (header.p_flags & PF_X) != 0 && header.p_memsz != 0;
            });
            require(executable != object.headers.end(), "loaded ELF lacks identifiable executable segment");
            require(identify(reinterpret_cast<const void *>(add(object.bias, executable->p_vaddr)), object.identity),
                    "cannot identify loaded ELF file");
            object.identity.base = object.bias;
            object.read(object.identity.path, false);
        }
        std::ifstream maps("/proc/self/maps");
        require(maps.is_open(), "cannot inspect loaded ELF protections");
        std::string line;
        std::size_t bytes = 0;
        std::size_t count = 0;
        while (std::getline(maps, line)) {
            require(++count <= 65536 && line.size() <= KMetadata - bytes, "loaded mapping metadata exceeds bounds");
            bytes += line.size();
            std::istringstream stream(line);
            std::string range, permissions, offset, device;
            unsigned long long inode = 0;
            require(static_cast<bool>(stream >> range >> permissions >> offset >> device >> inode),
                    "malformed loaded mapping metadata");
            const auto dash = range.find('-');
            const auto colon = device.find(':');
            require(dash != std::string::npos && colon != std::string::npos && permissions.size() >= 3,
                    "malformed loaded mapping range");
            const auto begin = std::stoull(range.substr(0, dash), nullptr, 16);
            const auto end = std::stoull(range.substr(dash + 1), nullptr, 16);
            require(begin < end, "invalid loaded mapping range");
            const auto mapped_device = makedev(std::stoul(device.substr(0, colon), nullptr, 16),
                                               std::stoul(device.substr(colon + 1), nullptr, 16));
            const unsigned flags = (permissions[0] == 'r' ? PF_R : 0) | (permissions[1] == 'w' ? PF_W : 0) |
                                   (permissions[2] == 'x' ? PF_X : 0);
            for (auto &object : objects) {
                if (inode != 0 && object.identity.device == mapped_device && object.identity.inode == inode) {
                    object.mapped_ranges.push_back({begin, end, flags});
                }
                else if (inode == 0 && object.identity.inode != 0 && (flags & PF_X) == 0) {
                    for (const auto &segment : object.headers) {
                        if (segment.p_type != PT_LOAD || segment.p_memsz <= segment.p_filesz) {
                            continue;
                        }
                        const auto zero_begin = add(add(object.bias, segment.p_vaddr), segment.p_filesz);
                        const auto zero_end = add(add(object.bias, segment.p_vaddr), segment.p_memsz);
                        const auto intersection_begin = std::max<std::uint64_t>(begin, zero_begin);
                        const auto intersection_end = std::min<std::uint64_t>(end, zero_end);
                        if (intersection_begin < intersection_end) {
                            object.mapped_ranges.push_back({intersection_begin, intersection_end, flags});
                        }
                    }
                }
            }
        }
        require(maps.eof(), "cannot complete loaded mapping metadata");
    }
    bool unchanged() const
    {
        Snapshot next;
        next.capture(false);
        if (next.adds != adds || next.subs != subs || next.objects.size() != objects.size()) {
            return false;
        }
        for (std::size_t i = 0; i < objects.size(); ++i) {
            const auto &first = objects[i];
            const auto &second = next.objects[i];
            if (first.bias != second.bias || first.loader_name != second.loader_name ||
                first.headers.size() != second.headers.size() ||
                std::memcmp(first.headers.data(), second.headers.data(), first.headers.size() * sizeof(Elf64_Phdr)) !=
                    0) {
                return false;
            }
        }
        return true;
    }
    std::size_t owner(const void *address, unsigned flags) const
    {
        std::size_t result = objects.size();
        for (std::size_t i = 0; i < objects.size(); ++i) {
            if (objects[i].identity.inode != 0 &&
                objects[i].contains(reinterpret_cast<std::uintptr_t>(address), 1, flags)) {
                require(result == objects.size(), "ELF address has ambiguous owner");
                result = i;
            }
        }
        require(result != objects.size(), "ELF address has no file-backed owner");
        return result;
    }
    std::size_t dependency(const std::string &name) const
    {
        require(!name.empty() && name.find('$') == std::string::npos &&
                    (name.front() == '/' || name.find('/') == std::string::npos),
                "unsupported ELF dependency path");
        std::size_t found = objects.size();
        for (std::size_t i = 0; i < objects.size(); ++i) {
            const auto &object = objects[i];
            if (object.identity.inode == 0) {
                continue;
            }
            const bool match = name.front() == '/' ? sameFile(name, object.identity.device, object.identity.inode)
                                                   : object.soname == name ||
                                                         std::filesystem::path(object.loader_name).filename() == name ||
                                                         std::filesystem::path(object.identity.path).filename() == name;
            if (match) {
                require(found == objects.size(), "ambiguous ELF dependency identity");
                found = i;
            }
        }
        require(found != objects.size(), "missing ELF dependency identity");
        return found;
    }
    std::set<std::size_t> closure(std::size_t root) const
    {
        std::set<std::size_t> result;
        std::vector<std::size_t> pending{root};
        std::size_t edges = 0;
        while (!pending.empty()) {
            const auto index = pending.back();
            pending.pop_back();
            if (!result.insert(index).second) {
                continue;
            }
            for (const auto &needed : objects[index].needed) {
                require(++edges <= KEdges, "ELF dependency walk exceeds bounds");
                pending.push_back(dependency(needed));
            }
        }
        return result;
    }
    std::set<std::size_t> roots() const
    {
        std::size_t root = objects.size();
        for (std::size_t i = 0; i < objects.size(); ++i) {
            if (objects[i].identity.inode != 0 && mainExecutable(objects[i].identity)) {
                require(root == objects.size() && objects[i].loader_name.empty(), "ambiguous main executable identity");
                root = i;
            }
        }
        require(root != objects.size(), "main executable absent from loader namespace");
        return closure(root);
    }
};

inline bool sparkObject(const Object &object)
{
    const auto name = std::filesystem::path(object.identity.path).filename().string();
    return name == "endstone_spark.so" || (name.starts_with("endstone_spark-") && name.ends_with(".so"));
}

inline std::string initialPreload()
{
    constexpr std::string_view key = "LD_PRELOAD=";
    constexpr std::size_t max_environment = 2 * 1024 * 1024;
    constexpr std::size_t max_preload = 64 * 1024;
    std::ifstream environment("/proc/self/environ", std::ios::binary);
    if (!environment) {
        return {};
    }
    std::string value;
    std::size_t matched = 0;
    bool ignored = false;
    bool capturing = false;
    bool found = false;
    bool boundary = true;
    char character = 0;
    std::size_t bytes = 0;
    while (environment.get(character)) {
        if (++bytes > max_environment) {
            return {};
        }
        if (character == '\0') {
            if (capturing) {
                found = true;
            }
            matched = 0;
            ignored = false;
            capturing = false;
            boundary = true;
            continue;
        }
        boundary = false;
        if (capturing) {
            if (value.size() == max_preload) {
                return {};
            }
            value += character;
        }
        else if (!ignored) {
            if (character != key[matched]) {
                ignored = true;
            }
            else if (++matched == key.size()) {
                if (found) {
                    return {};
                }
                capturing = true;
            }
        }
    }
    return environment.eof() && boundary && found ? value : std::string{};
}

struct Admission {
    Snapshot snapshot;
    std::set<std::size_t> resident;
    std::set<std::size_t> dependencies;
    std::size_t spark = 0;

    explicit Admission(const void *anchor)
    {
        snapshot.capture();
        resident = snapshot.roots();
        spark = snapshot.owner(anchor, PF_R);
        for (const auto index : resident) {
            require(!sparkObject(snapshot.objects[index]), "startup dependency closure includes Spark");
        }
    }
    bool preloadedAllocator(std::size_t index) const
    {
        const auto &object = snapshot.objects[index];
        const auto name = std::filesystem::path(object.identity.path).filename().string();
        const auto allocator = [](const std::string &value) {
            for (const char *prefix : {"libjemalloc.so", "libmimalloc.so"}) {
                const std::string_view stem(prefix);
                if (value == stem) {
                    return true;
                }
                if (value.starts_with(stem) && value.size() > stem.size() + 1 && value[stem.size()] == '.' &&
                    std::all_of(value.begin() + stem.size() + 1, value.end(), [](char character) {
                        return (character >= '0' && character <= '9') || character == '.';
                    })) {
                    return true;
                }
            }
            return false;
        };
        if (!allocator(name) || object.identity.inode == 0) {
            return false;
        }
        const auto preload = initialPreload();
        if (preload.empty()) {
            return false;
        }
        bool listed = false;
        for (const char *first = preload.c_str(); *first != '\0';) {
            while (*first == ' ' || *first == ':') {
                ++first;
            }
            const char *last = first;
            while (*last != '\0' && *last != ' ' && *last != ':') {
                ++last;
            }
            if (last != first) {
                const std::string entry(first, last);
                listed |= entry.find('/') == std::string::npos
                            ? entry == object.soname || entry == std::filesystem::path(object.loader_name).filename()
                            : sameFile(entry, object.identity.device, object.identity.inode);
            }
            first = last;
        }
        if (!listed || index >= spark) {
            return false;
        }
        const auto dependencies = snapshot.closure(index);
        return std::none_of(dependencies.begin(), dependencies.end(), [&](std::size_t dependency) {
            return dependency == spark || sparkObject(snapshot.objects[dependency]);
        });
    }
    void helperDependencies(const Object &helper)
    {
        dependencies.clear();
        for (const auto &needed : helper.needed) {
            const auto closure = snapshot.closure(snapshot.dependency(needed));
            dependencies.insert(closure.begin(), closure.end());
        }
        for (const auto index : dependencies) {
            require(resident.contains(index) && index != spark && !sparkObject(snapshot.objects[index]),
                    "helper dependency is outside startup closure");
        }
    }
    bool versionMatches(const Object &requester, std::size_t requested, const Object &provider,
                        std::size_t defined) const
    {
        const auto wanted = requester.versions[requested] & 0x7fff;
        const auto offered = provider.versions[defined] & 0x7fff;
        if (wanted <= VER_NDX_GLOBAL) {
            return offered == VER_NDX_GLOBAL ||
                   (offered > VER_NDX_GLOBAL && (provider.versions[defined] & 0x8000) == 0);
        }
        if (offered <= VER_NDX_GLOBAL) {
            return false;
        }
        const auto &first = requester.version_names.at(wanted);
        const auto &second = provider.version_names.at(offered);
        return first.name == second.name && first.hash == second.hash;
    }
    struct Definition {
        std::size_t object;
        std::size_t symbol;
    };
    std::vector<Definition> definitions(const Object &helper, std::size_t index) const
    {
        const auto &requested = helper.symbols[index];
        require(requested.st_shndx == SHN_UNDEF && ELF64_ST_BIND(requested.st_info) == STB_GLOBAL &&
                    ELF64_ST_VISIBILITY(requested.st_other) == STV_DEFAULT,
                "helper external import requires ordinary strong binding");
        const auto name = helper.string(requested.st_name);
        std::vector<Definition> result;
        std::size_t walks = 0;
        for (const auto object_index : dependencies) {
            const auto &object = snapshot.objects[object_index];
            for (std::size_t symbol_index = 1; symbol_index < object.symbols.size(); ++symbol_index) {
                require(++walks <= KSymbols, "helper symbol lookup exceeds bounds");
                const auto &symbol = object.symbols[symbol_index];
                const auto binding = ELF64_ST_BIND(symbol.st_info);
                const auto visibility = ELF64_ST_VISIBILITY(symbol.st_other);
                if (!object.lookup_symbols[symbol_index] || symbol.st_shndx == SHN_UNDEF ||
                    symbol.st_shndx >= SHN_LORESERVE || (visibility != STV_DEFAULT && visibility != STV_PROTECTED) ||
                    (binding != STB_GLOBAL && binding != STB_WEAK && binding != STB_GNU_UNIQUE) ||
                    object.string(symbol.st_name) != name || !versionMatches(helper, index, object, symbol_index)) {
                    continue;
                }
                require(binding != STB_GNU_UNIQUE, "helper import resolves to GNU unique definition");
                const auto type = ELF64_ST_TYPE(symbol.st_info);
                const auto requested_type = ELF64_ST_TYPE(requested.st_info);
                if ((requested_type == STT_FUNC && (type == STT_FUNC || type == STT_GNU_IFUNC)) ||
                    (requested_type == STT_OBJECT && type == STT_OBJECT) ||
                    (requested_type == STT_NOTYPE &&
                     (type == STT_FUNC || type == STT_OBJECT || type == STT_GNU_IFUNC))) {
                    result.push_back({object_index, symbol_index});
                }
            }
        }
        require(!result.empty(), "helper import has no approved versioned definition");
        const bool ifunc = std::any_of(result.begin(), result.end(), [&](const auto &definition) {
            return ELF64_ST_TYPE(snapshot.objects[definition.object].symbols[definition.symbol].st_info) ==
                   STT_GNU_IFUNC;
        });
        require(!ifunc || result.size() == 1, "helper IFUNC definition is ambiguous");
        return result;
    }
    void preflight(Object &helper)
    {
        helperDependencies(helper);
        for (std::size_t i = 1; i < helper.symbols.size(); ++i) {
            if (helper.symbols[i].st_shndx == SHN_UNDEF) {
                definitions(helper, i);
            }
        }
        require(snapshot.unchanged(), "loader changed during helper preflight");
    }
    void bindings(const Object &helper)
    {
        helperDependencies(helper);
        File memory("/proc/self/mem");
        for (const auto &relocation : helper.relocations) {
            const auto kind = ELF64_R_TYPE(relocation.r_info);
            if (kind == R_X86_64_NONE) {
                continue;
            }
            require(helper.contains(add(helper.bias, relocation.r_offset), 8, PF_R) &&
                        ((kind != R_X86_64_GLOB_DAT && kind != R_X86_64_JUMP_SLOT) ||
                         !helper.contains(add(helper.bias, relocation.r_offset), 8, PF_W)),
                    "helper relocation destination is not protected readable memory");
            const auto actual = memory.read<std::uintptr_t>(add(helper.bias, relocation.r_offset));
            const auto symbol_index = ELF64_R_SYM(relocation.r_info);
            if (kind == R_X86_64_RELATIVE) {
                const auto expected = signedAdd(helper.bias, relocation.r_addend);
                require(actual == expected && helper.contains(expected, 1, PF_R), "helper relative binding mismatch");
                continue;
            }
            const auto &symbol = helper.symbols[symbol_index];
            require(kind == R_X86_64_64 || relocation.r_addend == 0, "nonzero helper PLT/GOT addend");
            const auto addend = kind == R_X86_64_64 ? relocation.r_addend : 0;
            if (symbol.st_shndx != SHN_UNDEF) {
                const auto expected = signedAdd(add(helper.bias, symbol.st_value), addend);
                require(actual == expected && helper.contains(expected, 1, PF_R), "helper local binding mismatch");
                continue;
            }
            bool accepted = false;
            for (const auto &definition : definitions(helper, symbol_index)) {
                const auto &object = snapshot.objects[definition.object];
                const auto &provided = object.symbols[definition.symbol];
                const auto type = ELF64_ST_TYPE(provided.st_info);
                if (type == STT_GNU_IFUNC) {
                    require(addend == 0, "helper IFUNC addend is unsupported");
                    const auto target = snapshot.owner(reinterpret_cast<const void *>(actual), PF_R | PF_X);
                    accepted = resident.contains(target) && target != spark && !sparkObject(snapshot.objects[target]);
                }
                else {
                    const auto expected = signedAdd(add(object.bias, provided.st_value), addend);
                    const auto flags = type == STT_FUNC ? PF_R | PF_X : PF_R;
                    accepted = actual == expected && object.contains(expected, 1, flags) &&
                               (kind != R_X86_64_JUMP_SLOT || type == STT_FUNC);
                }
                if (accepted) {
                    break;
                }
            }
            if (!accepted) {
                throw std::runtime_error("helper external binding differs from approved definition: " +
                                         helper.string(symbol.st_name));
            }
        }
        require(snapshot.unchanged(), "loader changed during helper binding validation");
    }
};

}  // namespace spark::gateway::elf

#endif
