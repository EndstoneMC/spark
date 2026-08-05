#include "sampler/symbol_guess.h"

#include <string_view>
#include <utility>

namespace spark {
namespace {

GuessResult resultFromLabel(std::uint64_t function_rva, std::string label,
                            std::uint32_t evidence_count = 1) {
  GuessResult result;
  result.function_rva = function_rva;
  result.label = std::move(label);
  result.evidence_count = result.label.empty() ? 0 : evidence_count;
  struct Prefix {
    std::string_view text;
    GuessKind kind;
    Confidence confidence;
  };
  static constexpr Prefix prefixes[] = {
      {"rtti: ", GuessKind::Rtti, Confidence::High},
      {"rtti?: ", GuessKind::Rtti, Confidence::Medium},
      {"str: ", GuessKind::String, Confidence::High},
      {"str?: ", GuessKind::String, Confidence::Medium},
      {"vtable: ", GuessKind::Vtable, Confidence::High},
      {"vtable?: ", GuessKind::Vtable, Confidence::Medium},
      {"thunk: ", GuessKind::Thunk, Confidence::High},
      {"thunk?: ", GuessKind::Thunk, Confidence::Medium},
      {"call: ", GuessKind::Call, Confidence::High},
      {"call?: ", GuessKind::Call, Confidence::Medium},
      {"import: ", GuessKind::Import, Confidence::High},
      {"import?: ", GuessKind::Import, Confidence::Medium},
      {"context?: ", GuessKind::Context, Confidence::Low},
  };
  for (const Prefix &prefix : prefixes) {
    if (result.label.starts_with(prefix.text)) {
      result.kind = prefix.kind;
      result.confidence = prefix.confidence;
      break;
    }
  }
  if (!result.label.empty() && result.kind == GuessKind::None) {
    result.confidence = Confidence::Low;
  }
  return result;
}

std::unordered_map<std::uint64_t, std::string> labelsOnly(
    const std::unordered_map<std::uint64_t, GuessResult> &results) {
  std::unordered_map<std::uint64_t, std::string> labels;
  labels.reserve(results.size());
  for (const auto &[rva, result] : results) {
    if (!result.label.empty()) {
      labels.emplace(rva, result.label);
    }
  }
  return labels;
}

} // namespace
} // namespace spark

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

std::unordered_map<std::uint64_t, GuessResult>
analyzeMainModuleSymbols(std::span<const std::uint64_t> rvas) {
  const auto labels = symbol_guess::windows::guessCurrentModuleSymbols(rvas);
  std::unordered_map<std::uint64_t, GuessResult> results;
  results.reserve(labels.size());
  for (const auto &[rva, label] : labels) {
    results.emplace(rva, resultFromLabel(rva, label));
  }
  return results;
}

} // namespace spark

#elif defined(__linux__) && defined(__x86_64__)

#include "sampler/symbol_guess_evidence.h"
#include "sampler/symbol_guess_dwarf.h"
#include "sampler/symbol_guess_linux.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
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
                            std::uint64_t function_rva,
                            std::size_t *decoded_instructions) {
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
        if (decoded_instructions != nullptr) {
          ++*decoded_instructions;
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

using FunctionRange = symbol_guess::dwarf::FunctionRange;

struct GuessTable {
  std::vector<FunctionRange> ranges;
  std::unordered_map<std::uint64_t, std::string> labels;
  symbol_guess::dwarf::ParseStats range_stats;
  symbol_guess::linux::BuildStats stats;
};

std::mutex published_stats_mutex;
symbol_guess::linux::BuildStats published_stats;

void publishStats(const symbol_guess::linux::BuildStats &stats) {
  std::scoped_lock lock(published_stats_mutex);
  published_stats = stats;
}

symbol_guess::linux::BuildStats readPublishedStats() {
  std::scoped_lock lock(published_stats_mutex);
  return published_stats;
}

// Bounds-checked read-only view of the main executable, addressed by RVA
// (offset from the ELF load bias) so the interface matches the PE side.
class ImageView : public symbol_guess::dwarf::ImageView {
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

  std::size_t imageBytes() const {
    std::size_t total = 0;
    for (const Section &section : sections_) {
      const std::uint64_t size = section.end - section.begin;
      if (size > (std::numeric_limits<std::size_t>::max)() - total) {
        return (std::numeric_limits<std::size_t>::max)();
      }
      total += static_cast<std::size_t>(size);
    }
    return total;
  }

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

  bool read(std::uint64_t rva, void *out,
            std::size_t length) const override {
    if (out == nullptr || sectionContaining(rva, length) == nullptr) {
      return false;
    }
    std::memcpy(out, at(rva), length);
    return true;
  }

  bool executable(std::uint64_t rva,
                  std::size_t length) const override {
    const Section *section = sectionContaining(rva, length);
    return section != nullptr && section->executable;
  }

  std::uint64_t readableEnd(std::uint64_t rva) const override {
    const Section *section = sectionContaining(rva, 1);
    return section != nullptr ? section->end : 0;
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

void collectFunctions(const ImageView &img, GuessTable &table) {
  std::uint64_t text_base = 0;
  for (const Section &section : img.sections()) {
    if (section.executable && (text_base == 0 || section.begin < text_base)) {
      text_base = section.begin;
    }
  }
  table.ranges = symbol_guess::dwarf::parseEhFrameHeader(
      img, img.ehFrameHdr(), img.ehFrameHdrSize(), {text_base, 0, true, false},
      &table.range_stats);
  table.stats.table_entries = table.range_stats.table_entries;
  table.stats.eh_frame_records = table.range_stats.eh_frame_records;
  table.stats.function_ranges = table.range_stats.function_ranges;
  table.stats.rejected_ranges = table.range_stats.rejected_entries;
  table.stats.duplicate_ranges = table.range_stats.duplicate_ranges;
  table.stats.overlap_ranges = table.range_stats.overlap_ranges;
  table.stats.unindexed_ranges = table.range_stats.unindexed_ranges;
  table.stats.gap_ranges = table.range_stats.gap_ranges;
  table.stats.gap_bytes = table.range_stats.gap_bytes;
}

const FunctionRange *functionContaining(const GuessTable &table,
                                        std::uint64_t rva) {
  return symbol_guess::dwarf::functionContaining(table.ranges, rva);
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
      ++table.stats.vtables;
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
        ++table.stats.vtable_candidates;
      }
    }
  }
  for (auto &[function, evidence] : candidates) {
    std::string label = symbol_guess::chooseVtableLabel(std::move(evidence));
    if (label.empty()) {
      ++table.stats.vtable_conflicts;
      continue;
    }
    table.labels.emplace(function, std::move(label));
    ++table.stats.vtable_labels;
  }
}

struct StringCandidate {
  std::uint64_t target = 0;
  std::string value;
  int score = 0;
};

std::vector<StringCandidate> decodeStrings(const ImageView &img,
                                           const FunctionRange &function,
                                           symbol_guess::linux::BuildStats &stats) {
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
       symbol_guess::linux::decodeRipRelativeLeaTargets(
           code, function.begin, &stats.decoded_instructions)) {
    const Section *target_section = img.sectionContaining(target, 1);
    if (target_section == nullptr || target_section->executable) {
      continue;
    }
    std::string value = readCString(img, target, 180);
    const int score = symbol_guess::scoreStringHint(value);
    if (score >= symbol_guess::kMinimumStringHintScore) {
      candidates.push_back({target, std::move(value), score});
      ++stats.string_candidates;
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
    const std::uint8_t *cursor = bytes + 1;
    const std::uint8_t *end = bytes + (section.end - section.begin);
    while (cursor + 6 <= end) {
      const auto remaining = static_cast<std::size_t>(end - cursor - 5);
      const auto *opcode = static_cast<const std::uint8_t *>(
          std::memchr(cursor, 0x8d, remaining));
      if (opcode == nullptr) {
        break;
      }
      cursor = opcode + 1;
      const std::uint8_t rex = opcode[-1];
      if (rex < 0x48 || rex > 0x4f || (opcode[1] & 0xc7) != 0x05) {
        continue;
      }
      std::int32_t displacement = 0;
      std::memcpy(&displacement, opcode + 2, 4);
      const std::uint64_t rva =
          section.begin + static_cast<std::uint64_t>(opcode - bytes - 1);
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

std::unordered_map<std::uint64_t, GuessResult>
guessBatch(std::span<const std::uint64_t> rvas) {
  std::unordered_map<std::uint64_t, GuessResult> out;
  if (rvas.empty()) {
    return out;
  }
  const GuessTable &table = guessTable();
  // Index construction is reported separately. Batch time measures only the
  // per-export work that scales with the unique sampled function set.
  const auto started = std::chrono::steady_clock::now();
  symbol_guess::linux::BuildStats batch = table.stats;
  out.reserve(rvas.size());
  auto finish = [&](std::unordered_map<std::uint64_t, GuessResult> result) {
    batch.batch_microseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    publishStats(batch);
    return result;
  };
  if (table.ranges.empty()) {
    return finish(std::move(out));
  }
  ImageView img;
  if (!img.init()) {
    return finish(std::move(out));
  }

  std::map<std::uint64_t, std::vector<std::uint64_t>> root_inputs;
  for (std::uint64_t rva : rvas) {
    if (const FunctionRange *function = functionContaining(table, rva)) {
      root_inputs[function->root].push_back(rva);
      out.try_emplace(rva, resultFromLabel(function->root, {}, 0));
    }
  }
  batch.sampled_functions = root_inputs.size();

  std::map<std::uint64_t, std::vector<StringCandidate>> string_candidates;
  std::unordered_set<std::uint64_t> candidate_targets;
  for (const auto &[root, inputs] : root_inputs) {
    if (const auto label = table.labels.find(root);
        label != table.labels.end()) {
      for (std::uint64_t rva : inputs) {
        out[rva] = resultFromLabel(root, label->second);
      }
      continue;
    }
    const FunctionRange *function = functionContaining(table, root);
    if (function == nullptr) {
      continue;
    }
    std::vector<StringCandidate> candidates =
        decodeStrings(img, *function, batch);
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
      if (refs != references.end() && refs->second.size() > 1) {
        ++batch.shared_strings;
      }
    }
    if (label.empty()) {
      continue;
    }
    ++batch.string_labels;
    for (std::uint64_t rva : root_inputs.at(root)) {
      out[rva] = resultFromLabel(root, label);
    }
  }
  return finish(std::move(out));
}

GuessTable buildTable() {
  const auto started = std::chrono::steady_clock::now();
  GuessTable table;
  ImageView img;
  if (!img.init()) {
    table.stats.initialized = true;
    table.stats.build_microseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    publishStats(table.stats);
    return table;
  }
  table.stats.image_bytes = img.imageBytes();
  collectFunctions(img, table);
  if (table.ranges.empty()) {
    table.stats.initialized = true;
    table.stats.build_microseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    publishStats(table.stats);
    return table;
  }
  collectVtableLabels(img, table);
  table.stats.approximate_bytes =
      table.ranges.capacity() * sizeof(FunctionRange) +
      table.labels.size() *
          (sizeof(decltype(table.labels)::value_type) + sizeof(void *) * 2);
  for (const auto &[root, label] : table.labels) {
    (void)root;
    table.stats.approximate_bytes += label.capacity();
  }
  table.stats.initialized = true;
  table.stats.build_microseconds = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - started)
          .count());
  publishStats(table.stats);
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
  return it != guesses.end() ? it->second.label : std::string{};
}

std::unordered_map<std::uint64_t, std::string>
guessMainModuleSymbols(std::span<const std::uint64_t> rvas) {
  return labelsOnly(guessBatch(rvas));
}

std::unordered_map<std::uint64_t, GuessResult>
analyzeMainModuleSymbols(std::span<const std::uint64_t> rvas) {
  return guessBatch(rvas);
}

symbol_guess::linux::BuildStats symbol_guess::linux::currentModuleStats() {
  return readPublishedStats();
}

} // namespace spark

#else

namespace spark {

std::string guessMainModuleSymbol(std::uint64_t) { return {}; }

std::unordered_map<std::uint64_t, std::string>
guessMainModuleSymbols(std::span<const std::uint64_t>) {
  return {};
}

std::unordered_map<std::uint64_t, GuessResult>
analyzeMainModuleSymbols(std::span<const std::uint64_t>) {
  return {};
}

} // namespace spark

#endif
