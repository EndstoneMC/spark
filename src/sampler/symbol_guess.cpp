#include "sampler/symbol_guess.h"

#if defined(_WIN32)

#include "sampler/symbol_guess_windows.h"

namespace spark {

std::string guessMainModuleSymbol(std::uint64_t rva) {
  const auto guesses =
      symbol_guess::windows::guessCurrentModuleSymbols(std::span(&rva, 1));
  const auto it = guesses.find(rva);
  return it != guesses.end() ? it->second : std::string{};
}

std::unordered_map<std::uint64_t, std::string>
guessMainModuleSymbols(std::span<const std::uint64_t> rvas) {
  return symbol_guess::windows::guessCurrentModuleSymbols(rvas);
}

} // namespace spark

#elif defined(__linux__) && defined(__x86_64__)

#include "sampler/symbol_guess_evidence.h"
#include "sampler/symbol_guess_linux.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <cxxabi.h>
#include <distorm.h>
#include <link.h>
#include <mnemonics.h>

namespace spark {

namespace symbol_guess::linux {

std::vector<std::uint64_t>
decodeRipRelativeLeaTargets(std::span<const std::uint8_t> code,
                            std::uint64_t function_rva) {
  if (code.empty() || code.size() > static_cast<std::size_t>(
                                        (std::numeric_limits<int>::max)())) {
    return {};
  }

  std::vector<std::size_t> work{0};
  std::unordered_set<std::size_t> visited;
  std::set<std::uint64_t> targets;
  while (!work.empty()) {
    std::size_t cursor = work.back();
    work.pop_back();
    while (cursor < code.size()) {
      _CodeInfo info{};
      info.codeOffset = function_rva + cursor;
      info.code = code.data() + cursor;
      info.codeLen = static_cast<int>(code.size() - cursor);
      info.dt = Decode64Bits;
      info.features = DF_STOP_ON_FLOW_CONTROL | DF_STOP_ON_UNDECODEABLE;
      _DInst instructions[64]{};
      unsigned used = 0;
      const _DecodeResult result =
          distorm_decompose64(&info, instructions, 64, &used);
      if ((result == DECRES_INPUTERR || result == DECRES_NONE) || used == 0) {
        break;
      }

      bool stop = false;
      for (unsigned i = 0; i < used; ++i) {
        const _DInst &instruction = instructions[i];
        if (instruction.flags == FLAG_NOT_DECODABLE || instruction.size == 0 ||
            instruction.addr < function_rva ||
            instruction.addr - function_rva >= code.size()) {
          stop = true;
          break;
        }
        const std::size_t offset =
            static_cast<std::size_t>(instruction.addr - function_rva);
        if (!visited.insert(offset).second) {
          stop = true;
          break;
        }
        if (instruction.opcode == I_LEA &&
            (instruction.flags & FLAG_RIP_RELATIVE) != 0) {
          targets.insert(INSTRUCTION_GET_RIP_TARGET(&instruction));
        }

        const unsigned flow = META_GET_FC(instruction.meta);
        if (flow == FC_CND_BRANCH || flow == FC_UNC_BRANCH) {
          for (const _Operand &operand : instruction.ops) {
            if (operand.type != O_PC) {
              continue;
            }
            const std::uint64_t target = INSTRUCTION_GET_TARGET(&instruction);
            if (target >= function_rva && target - function_rva < code.size()) {
              work.push_back(static_cast<std::size_t>(target - function_rva));
            }
            break;
          }
        }
        cursor = offset + instruction.size;
        if (flow == FC_RET || flow == FC_SYS || flow == FC_UNC_BRANCH ||
            flow == FC_INT || flow == FC_HLT) {
          stop = true;
        }
      }
      if (stop) {
        break;
      }
    }
  }
  return {targets.begin(), targets.end()};
}

} // namespace symbol_guess::linux

namespace {

struct Section {
  std::uint64_t begin = 0;
  std::uint64_t end = 0;
  bool executable = false;
};

// One function extent recovered from .eh_frame. Unlike PE, FDEs are never
// chained, so `root` always equals `begin`; it is kept for symmetry with the
// Windows table.
struct FunctionRange {
  std::uint64_t begin = 0;
  std::uint64_t end = 0;
  std::uint64_t root = 0;
};

struct GuessTable {
  std::vector<FunctionRange> ranges;
  std::unordered_map<std::uint64_t, std::string> labels;
};

// Bounds-checked read-only view of the main executable, addressed by RVA
// (offset from the ELF load bias) so the interface matches the PE side.
class ImageView {
public:
  bool init() {
    Collect collect{};
    dl_iterate_phdr(&ImageView::onObject, &collect);
    if (!collect.found) {
      return false;
    }
    bias_ = collect.bias;
    sections_ = std::move(collect.sections);
    eh_frame_hdr_ = collect.eh_frame_hdr;
    eh_frame_hdr_size_ = collect.eh_frame_hdr_size;
    std::sort(
        sections_.begin(), sections_.end(),
        [](const Section &a, const Section &b) { return a.begin < b.begin; });
    return !sections_.empty();
  }

  std::uint64_t bias() const { return bias_; }

  const std::vector<Section> &sections() const { return sections_; }

  std::uint64_t ehFrameHdr() const { return eh_frame_hdr_; }

  std::uint64_t ehFrameHdrSize() const { return eh_frame_hdr_size_; }

  const Section *sectionContaining(std::uint64_t rva,
                                   std::uint64_t length) const {
    for (const Section &s : sections_) {
      if (rva >= s.begin && rva < s.end && length <= s.end - rva) {
        return &s;
      }
    }
    return nullptr;
  }

  const std::uint8_t *at(std::uint64_t rva) const {
    return reinterpret_cast<const std::uint8_t *>(bias_ + rva);
  }

  // True when `pointer` is an absolute virtual address inside a mapped segment.
  // For a non-PIE image the load bias is 0 and stored pointers already equal
  // the p_vaddr-based RVAs, so subtracting the bias is correct in both cases.
  bool toRva(std::uint64_t pointer, std::uint64_t &rva) const {
    if (pointer < bias_) {
      return false;
    }
    const std::uint64_t candidate = pointer - bias_;
    if (sectionContaining(candidate, 1) == nullptr) {
      return false;
    }
    rva = candidate;
    return true;
  }

private:
  struct Collect {
    bool found = false;
    std::uint64_t bias = 0;
    std::uint64_t eh_frame_hdr = 0;
    std::uint64_t eh_frame_hdr_size = 0;
    std::vector<Section> sections;
  };

  // The first entry dl_iterate_phdr reports is always the main executable.
  static int onObject(struct dl_phdr_info *info, std::size_t, void *data) {
    auto *collect = static_cast<Collect *>(data);
    collect->found = true;
    collect->bias = static_cast<std::uint64_t>(info->dlpi_addr);
    for (int i = 0; i < info->dlpi_phnum; ++i) {
      const ElfW(Phdr) &ph = info->dlpi_phdr[i];
      if (ph.p_type == PT_GNU_EH_FRAME) {
        collect->eh_frame_hdr = static_cast<std::uint64_t>(ph.p_vaddr);
        collect->eh_frame_hdr_size = static_cast<std::uint64_t>(ph.p_memsz);
        continue;
      }
      // p_filesz, not p_memsz: .bss has no file backing and reading it here
      // would walk uninitialized memory.
      if (ph.p_type != PT_LOAD || (ph.p_flags & PF_R) == 0 ||
          ph.p_filesz == 0) {
        continue;
      }
      collect->sections.push_back(
          {static_cast<std::uint64_t>(ph.p_vaddr),
           static_cast<std::uint64_t>(ph.p_vaddr) + ph.p_filesz,
           (ph.p_flags & PF_X) != 0});
    }
    return 1;
  }

  std::uint64_t bias_ = 0;
  std::uint64_t eh_frame_hdr_ = 0;
  std::uint64_t eh_frame_hdr_size_ = 0;
  std::vector<Section> sections_;
};

template <typename T>
bool readAt(const ImageView &img, std::uint64_t rva, T &out) {
  if (img.sectionContaining(rva, sizeof(T)) == nullptr) {
    return false;
  }
  std::memcpy(&out, img.at(rva), sizeof(T));
  return true;
}

// Reads a NUL-terminated printable-ASCII string of at most `maximum` bytes.
// Returns empty when unterminated, unprintable, or out of bounds.
std::string readCString(const ImageView &img, std::uint64_t rva,
                        std::uint64_t maximum) {
  const Section *section = img.sectionContaining(rva, 1);
  if (section == nullptr) {
    return {};
  }
  const std::uint64_t limit = std::min(maximum, section->end - rva);
  const char *p = reinterpret_cast<const char *>(img.at(rva));
  for (std::uint64_t i = 0; i < limit; ++i) {
    const auto c = static_cast<unsigned char>(p[i]);
    if (c == '\0') {
      return std::string(p, i);
    }
    if (c < 0x20 || c > 0x7e) {
      return {};
    }
  }
  return {};
}

// DWARF pointer encodings used by .eh_frame_hdr.
constexpr std::uint8_t kDwEhPeOmit = 0xff;
constexpr std::uint8_t kDwEhPeUleb128 = 0x01;
constexpr std::uint8_t kDwEhPeUdata2 = 0x02;
constexpr std::uint8_t kDwEhPeUdata4 = 0x03;
constexpr std::uint8_t kDwEhPeUdata8 = 0x04;
constexpr std::uint8_t kDwEhPeSleb128 = 0x09;
constexpr std::uint8_t kDwEhPeSdata2 = 0x0a;
constexpr std::uint8_t kDwEhPeSdata4 = 0x0b;
constexpr std::uint8_t kDwEhPeSdata8 = 0x0c;
constexpr std::uint8_t kDwEhPePcrel = 0x10;
constexpr std::uint8_t kDwEhPeDatarel = 0x30;

// Decodes one DWARF-encoded pointer at `rva`, advancing it. `base` is the
// datarel anchor (the .eh_frame_hdr start). Returns false on an unsupported
// encoding or an out-of-bounds read.
bool readEncoded(const ImageView &img, std::uint8_t encoding,
                 std::uint64_t base, std::uint64_t &rva, std::uint64_t &out) {
  if (encoding == kDwEhPeOmit) {
    return false;
  }
  const std::uint64_t start = rva;
  std::uint64_t value = 0;
  switch (encoding & 0x0f) {
  case kDwEhPeUdata2: {
    std::uint16_t raw = 0;
    if (!readAt(img, rva, raw)) {
      return false;
    }
    value = raw;
    rva += 2;
    break;
  }
  case kDwEhPeSdata2: {
    std::int16_t raw = 0;
    if (!readAt(img, rva, raw)) {
      return false;
    }
    value = static_cast<std::uint64_t>(static_cast<std::int64_t>(raw));
    rva += 2;
    break;
  }
  case kDwEhPeUdata4: {
    std::uint32_t raw = 0;
    if (!readAt(img, rva, raw)) {
      return false;
    }
    value = raw;
    rva += 4;
    break;
  }
  case kDwEhPeSdata4: {
    std::int32_t raw = 0;
    if (!readAt(img, rva, raw)) {
      return false;
    }
    value = static_cast<std::uint64_t>(static_cast<std::int64_t>(raw));
    rva += 4;
    break;
  }
  case kDwEhPeUdata8:
  case kDwEhPeSdata8: {
    std::uint64_t raw = 0;
    if (!readAt(img, rva, raw)) {
      return false;
    }
    value = raw;
    rva += 8;
    break;
  }
  case kDwEhPeUleb128:
  case kDwEhPeSleb128:
  default:
    return false; // absptr and LEB128 forms do not appear in a binary search
                  // table
  }

  if ((encoding & 0x70) == kDwEhPePcrel) {
    value += start;
  } else if ((encoding & 0x70) == kDwEhPeDatarel) {
    value += base;
  }
  out = value;
  return true;
}

// Walks the .eh_frame_hdr binary search table, whose entries are already sorted
// by initial location. Each entry gives a function start; the extent runs to
// the next start, which is what functionContaining needs. The FDE's own length
// is not decoded because .eh_frame parsing is far more fragile than this table.
void collectFunctions(const ImageView &img, GuessTable &table) {
  const std::uint64_t hdr = img.ehFrameHdr();
  if (hdr == 0 || img.ehFrameHdrSize() < 12) {
    return;
  }
  std::uint8_t header[4] = {};
  if (!readAt(img, hdr, header) || header[0] != 1) {
    return;
  }
  const std::uint8_t eh_frame_ptr_enc = header[1];
  const std::uint8_t fde_count_enc = header[2];
  const std::uint8_t table_enc = header[3];
  // Only the standard sorted 4-byte datarel table is understood.
  if (table_enc != (kDwEhPeDatarel | kDwEhPeSdata4)) {
    return;
  }

  std::uint64_t cursor = hdr + 4;
  std::uint64_t ignored = 0;
  if (!readEncoded(img, eh_frame_ptr_enc, hdr, cursor, ignored)) {
    return;
  }
  std::uint64_t count = 0;
  if (!readEncoded(img, fde_count_enc, hdr, cursor, count) || count == 0) {
    return;
  }
  constexpr std::uint64_t kMaximumEntries = 4u << 20;
  if (count > kMaximumEntries) {
    return;
  }

  table.ranges.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t i = 0; i < count; ++i) {
    std::uint64_t initial = 0;
    std::uint64_t fde_ignored = 0;
    if (!readEncoded(img, table_enc, hdr, cursor, initial) ||
        !readEncoded(img, table_enc, hdr, cursor, fde_ignored)) {
      break;
    }
    const Section *section = img.sectionContaining(initial, 1);
    if (section == nullptr || !section->executable) {
      continue;
    }
    table.ranges.push_back({initial, 0, initial});
  }

  std::sort(table.ranges.begin(), table.ranges.end(),
            [](const FunctionRange &a, const FunctionRange &b) {
              return a.begin < b.begin;
            });
  for (std::size_t i = 0; i < table.ranges.size(); ++i) {
    FunctionRange &range = table.ranges[i];
    const Section *section = img.sectionContaining(range.begin, 1);
    const std::uint64_t section_end =
        section != nullptr ? section->end : range.begin;
    const std::uint64_t next =
        i + 1 < table.ranges.size() ? table.ranges[i + 1].begin : section_end;
    range.end = std::min(next, section_end);
  }
  table.ranges.erase(
      std::remove_if(table.ranges.begin(), table.ranges.end(),
                     [](const FunctionRange &r) { return r.end <= r.begin; }),
      table.ranges.end());
}

const FunctionRange *functionContaining(const GuessTable &table,
                                        std::uint64_t rva) {
  auto it = std::upper_bound(table.ranges.begin(), table.ranges.end(), rva,
                             [](std::uint64_t value, const FunctionRange &r) {
                               return value < r.begin;
                             });
  if (it == table.ranges.begin()) {
    return nullptr;
  }
  --it;
  return rva < it->end ? &*it : nullptr;
}

// "N6detail11ChunkSourceE" -> "detail::ChunkSource". Uses the ABI demangler,
// which expects a mangled name, so the typeinfo string is prefixed with _Z TS.
std::string classNameFromTypeInfo(const std::string &mangled) {
  if (mangled.empty()) {
    return {};
  }
  // A leading '*' marks an indirect typeinfo name shared across objects.
  const std::string name = mangled[0] == '*' ? mangled.substr(1) : mangled;
  if (name.empty()) {
    return {};
  }
  int status = 0;
  char *demangled =
      abi::__cxa_demangle(("_ZTS" + name).c_str(), nullptr, nullptr, &status);
  if (status != 0 || demangled == nullptr) {
    std::free(demangled);
    return {};
  }
  std::string out(demangled);
  std::free(demangled);
  // __cxa_demangle on _ZTS<name> yields "typeinfo name for <class>".
  constexpr std::string_view kPrefix = "typeinfo name for ";
  if (out.rfind(kPrefix, 0) == 0) {
    out.erase(0, kPrefix.size());
  }
  return out;
}

// Returns the class name when `type_info_rva` holds a valid Itanium type_info:
// a vptr into a mapped segment followed by a pointer to a printable name.
std::string validateTypeInfo(const ImageView &img,
                             std::uint64_t type_info_rva) {
  std::uint64_t vptr = 0;
  if (!readAt(img, type_info_rva, vptr)) {
    return {};
  }
  std::uint64_t vptr_rva = 0;
  if (!img.toRva(vptr, vptr_rva)) {
    return {};
  }
  std::uint64_t name_pointer = 0;
  if (!readAt(img, type_info_rva + 8, name_pointer)) {
    return {};
  }
  std::uint64_t name_rva = 0;
  if (!img.toRva(name_pointer, name_rva)) {
    return {};
  }
  const std::string mangled = readCString(img, name_rva, 256);
  if (mangled.empty()) {
    return {};
  }
  std::string name = classNameFromTypeInfo(mangled);
  if (name.empty()) {
    return {};
  }
  if (name.size() > 64) {
    name.resize(61);
    name += "...";
  }
  return name;
}

// Scans data segments for Itanium vtables. Collect every owner before choosing
// a label: first-wins would silently assign shared implementations to whichever
// class happened to appear first in the image.
void collectVtableLabels(const ImageView &img, GuessTable &table) {
  std::unordered_map<std::uint64_t, std::vector<symbol_guess::VtableEvidence>>
      candidates;
  for (const Section &section : img.sections()) {
    if (section.executable || section.end - section.begin < 32) {
      continue;
    }
    const std::uint64_t start =
        std::max(section.begin + std::uint64_t{8},
                 (section.begin + std::uint64_t{15}) & ~std::uint64_t{7});
    for (std::uint64_t rva = start; rva + 24 <= section.end; rva += 8) {
      std::int64_t offset_to_top = 0;
      std::memcpy(&offset_to_top, img.at(rva - 8), 8);
      // Complete-object and secondary vtables use zero or a small
      // negative adjustment. Positive construction-vtable offsets and
      // implausibly large values are not stable ownership evidence.
      if (offset_to_top > 0 || offset_to_top < -(1ll << 24) ||
          (offset_to_top & 7) != 0) {
        continue;
      }
      std::uint64_t type_info_pointer = 0;
      std::memcpy(&type_info_pointer, img.at(rva), 8);
      std::uint64_t type_info_rva = 0;
      if (!img.toRva(type_info_pointer, type_info_rva)) {
        continue;
      }
      const std::string class_name = validateTypeInfo(img, type_info_rva);
      if (class_name.empty()) {
        continue;
      }
      const std::uint64_t vtable = rva + 8;
      for (std::uint64_t slot = 0; vtable + 8u * (slot + 1) <= section.end;
           ++slot) {
        std::uint64_t entry = 0;
        std::memcpy(&entry, img.at(vtable + 8u * slot), 8);
        std::uint64_t target = 0;
        if (!img.toRva(entry, target)) {
          break;
        }
        const Section *target_section = img.sectionContaining(target, 1);
        if (target_section == nullptr || !target_section->executable) {
          break;
        }
        const FunctionRange *fn = functionContaining(table, target);
        if (fn == nullptr) {
          continue;
        }
        candidates[fn->root].push_back({class_name,
                                        static_cast<std::uint32_t>(slot),
                                        offset_to_top != 0, false});
      }
    }
  }
  for (auto &[function, evidence] : candidates) {
    std::string label = symbol_guess::chooseVtableLabel(std::move(evidence));
    if (!label.empty()) {
      table.labels.emplace(function, std::move(label));
    }
  }
}

struct StringCandidate {
  std::uint64_t target = 0;
  std::string value;
  int score = 0;
};

std::vector<StringCandidate> decodeStrings(const ImageView &img,
                                           const FunctionRange &function) {
  const Section *section =
      img.sectionContaining(function.begin, function.end - function.begin);
  if (section == nullptr || !section->executable) {
    return {};
  }
  const auto code =
      std::span(img.at(function.begin),
                static_cast<std::size_t>(function.end - function.begin));
  std::vector<StringCandidate> candidates;
  for (std::uint64_t target :
       symbol_guess::linux::decodeRipRelativeLeaTargets(code, function.begin)) {
    const Section *target_section = img.sectionContaining(target, 1);
    if (target_section == nullptr || target_section->executable) {
      continue;
    }
    std::string value = readCString(img, target, 180);
    const int score = symbol_guess::scoreStringHint(value);
    if (score >= symbol_guess::kMinimumStringHintScore) {
      candidates.push_back({target, std::move(value), score});
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const StringCandidate &a, const StringCandidate &b) {
              if (a.score != b.score) {
                return a.score > b.score;
              }
              if (a.value != b.value) {
                return a.value < b.value;
              }
              return a.target < b.target;
            });
  return candidates;
}

// Verify uniqueness globally for only the candidate targets recovered from
// sampled functions. This byte-level pass can only add an apparent extra
// reference and suppress a label; it cannot create a label because candidates
// themselves came from the real instruction decoder.
void scanCandidateReferences(
    const ImageView &img, const GuessTable &table,
    const std::unordered_set<std::uint64_t> &targets,
    std::unordered_map<std::uint64_t, std::set<std::uint64_t>> &references) {
  for (const Section &section : img.sections()) {
    if (!section.executable || section.end - section.begin < 7) {
      continue;
    }
    const std::uint8_t *bytes = img.at(section.begin);
    for (std::uint64_t offset = 0; offset + 7 <= section.end - section.begin;
         ++offset) {
      if (bytes[offset] < 0x48 || bytes[offset] > 0x4f ||
          bytes[offset + 1] != 0x8d || (bytes[offset + 2] & 0xc7) != 0x05) {
        continue;
      }
      std::int32_t displacement = 0;
      std::memcpy(&displacement, bytes + offset + 3, 4);
      const std::uint64_t rva = section.begin + offset;
      const std::int64_t wide_target =
          static_cast<std::int64_t>(rva) + 7 + displacement;
      if (wide_target < 0) {
        continue;
      }
      const auto target = static_cast<std::uint64_t>(wide_target);
      if (!targets.contains(target)) {
        continue;
      }
      if (const FunctionRange *fn = functionContaining(table, rva)) {
        references[target].insert(fn->root);
      }
    }
  }
}

const GuessTable &guessTable();

std::unordered_map<std::uint64_t, std::string>
guessBatch(std::span<const std::uint64_t> rvas) {
  const GuessTable &table = guessTable();
  std::unordered_map<std::uint64_t, std::string> out;
  out.reserve(rvas.size());
  if (rvas.empty() || table.ranges.empty()) {
    return out;
  }
  ImageView img;
  if (!img.init()) {
    return out;
  }

  std::map<std::uint64_t, std::vector<std::uint64_t>> root_inputs;
  for (std::uint64_t rva : rvas) {
    if (const FunctionRange *function = functionContaining(table, rva)) {
      root_inputs[function->root].push_back(rva);
    }
  }

  std::map<std::uint64_t, std::vector<StringCandidate>> string_candidates;
  std::unordered_set<std::uint64_t> candidate_targets;
  for (const auto &[root, inputs] : root_inputs) {
    if (const auto label = table.labels.find(root);
        label != table.labels.end()) {
      for (std::uint64_t rva : inputs) {
        out.emplace(rva, label->second);
      }
      continue;
    }
    const FunctionRange *function = functionContaining(table, root);
    if (function == nullptr) {
      continue;
    }
    std::vector<StringCandidate> candidates = decodeStrings(img, *function);
    for (const StringCandidate &candidate : candidates) {
      candidate_targets.insert(candidate.target);
    }
    string_candidates.emplace(root, std::move(candidates));
  }

  std::unordered_map<std::uint64_t, std::set<std::uint64_t>> references;
  if (!candidate_targets.empty()) {
    scanCandidateReferences(img, table, candidate_targets, references);
  }
  for (const auto &[root, candidates] : string_candidates) {
    std::string label;
    for (const StringCandidate &candidate : candidates) {
      const auto refs = references.find(candidate.target);
      if (refs != references.end() && refs->second.size() == 1 &&
          *refs->second.begin() == root) {
        label =
            symbol_guess::formatStringHint(candidate.value, candidate.score);
        break;
      }
    }
    if (label.empty()) {
      continue;
    }
    for (std::uint64_t rva : root_inputs.at(root)) {
      out.emplace(rva, label);
    }
  }
  return out;
}

GuessTable buildTable() {
  GuessTable table;
  ImageView img;
  if (!img.init()) {
    return table;
  }
  collectFunctions(img, table);
  if (table.ranges.empty()) {
    return table;
  }
  collectVtableLabels(img, table);
  return table;
}

const GuessTable &guessTable() {
  static const GuessTable table = buildTable();
  return table;
}

} // namespace

std::string guessMainModuleSymbol(std::uint64_t rva) {
  const auto guesses = guessBatch(std::span(&rva, 1));
  const auto it = guesses.find(rva);
  return it != guesses.end() ? it->second : std::string{};
}

std::unordered_map<std::uint64_t, std::string>
guessMainModuleSymbols(std::span<const std::uint64_t> rvas) {
  return guessBatch(rvas);
}

} // namespace spark

#else

namespace spark {

std::string guessMainModuleSymbol(std::uint64_t) { return {}; }

std::unordered_map<std::uint64_t, std::string>
guessMainModuleSymbols(std::span<const std::uint64_t>) {
  return {};
}

} // namespace spark

#endif
