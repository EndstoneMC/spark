#include "sampler/symbol_guess_evidence.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>

namespace spark::symbol_guess {

namespace {

std::string lower(std::string_view value) {
  std::string out(value);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

bool containsAny(std::string_view value,
                 std::initializer_list<std::string_view> needles) {
  return std::ranges::any_of(needles, [value](std::string_view needle) {
    return value.find(needle) != value.npos;
  });
}

std::string_view sourceName(EvidenceSource source) {
  switch (source) {
  case EvidenceSource::Rtti:
    return "rtti";
  case EvidenceSource::String:
    return "str";
  case EvidenceSource::Vtable:
    return "vtable";
  case EvidenceSource::Thunk:
    return "thunk";
  }
  return "guess";
}

} // namespace

void InheritanceMap::addBase(std::string_view derived, std::string_view base) {
  parents_[std::string(derived)].insert(std::string(base));
}

bool InheritanceMap::isAncestor(std::string_view ancestor,
                                std::string_view descendant) const {
  if (ancestor == descendant) {
    return true;
  }
  std::unordered_set<std::string> visited;
  std::vector<std::string> stack{std::string(descendant)};
  while (!stack.empty()) {
    std::string current = std::move(stack.back());
    stack.pop_back();
    if (!visited.insert(current).second) {
      continue;
    }
    auto it = parents_.find(current);
    if (it == parents_.end()) {
      continue;
    }
    for (const std::string &parent : it->second) {
      if (parent == ancestor) {
        return true;
      }
      if (!visited.contains(parent)) {
        stack.push_back(parent);
      }
    }
  }
  return false;
}

std::optional<std::string>
InheritanceMap::findCommonAncestor(const std::set<std::string> &classes) const {
  if (classes.empty()) {
    return std::nullopt;
  }
  // Compute the intersection of ancestor sets (including each class itself).
  std::unordered_set<std::string> common;
  bool first = true;
  for (const std::string &cls : classes) {
    std::unordered_set<std::string> ancestors;
    ancestors.insert(cls);
    std::vector<std::string> stack{cls};
    std::unordered_set<std::string> visited;
    while (!stack.empty()) {
      std::string current = std::move(stack.back());
      stack.pop_back();
      if (!visited.insert(current).second) {
        continue;
      }
      auto it = parents_.find(current);
      if (it == parents_.end()) {
        continue;
      }
      for (const std::string &parent : it->second) {
        ancestors.insert(parent);
        if (!visited.contains(parent)) {
          stack.push_back(parent);
        }
      }
    }
    if (first) {
      common = std::move(ancestors);
      first = false;
    } else {
      std::erase_if(common, [&ancestors](const std::string &s) {
        return !ancestors.contains(s);
      });
    }
    if (common.empty()) {
      return std::nullopt;
    }
  }
  if (common.size() == 1) {
    return *common.begin();
  }
  // Multiple common ancestors: find the most derived one (the one that no
  // other common ancestor is a parent of).
  for (const std::string &candidate : common) {
    bool is_most_derived = true;
    for (const std::string &other : common) {
      if (candidate != other && isAncestor(candidate, other)) {
        is_most_derived = false;
        break;
      }
    }
    if (is_most_derived) {
      return candidate;
    }
  }
  return std::nullopt;
}

std::string formatEvidenceLabel(EvidenceSource source, std::string_view message,
                                bool tentative) {
  if (message.empty()) {
    return {};
  }
  std::string out(sourceName(source));
  if (tentative) {
    out += '?';
  }
  out += ": ";
  out += message;
  return out;
}

std::string chooseVtableLabel(std::vector<VtableEvidence> evidence,
                              const InheritanceMap *inheritance) {
  std::sort(evidence.begin(), evidence.end(),
            [](const VtableEvidence &a, const VtableEvidence &b) {
              if (a.class_name != b.class_name) {
                return a.class_name < b.class_name;
              }
              if (a.slot != b.slot) {
                return a.slot < b.slot;
              }
              if (a.secondary != b.secondary) {
                return a.secondary < b.secondary;
              }
              return a.via_thunk < b.via_thunk;
            });
  evidence.erase(std::unique(evidence.begin(), evidence.end()), evidence.end());
  if (evidence.empty()) {
    return {};
  }

  std::set<std::string> classes;
  std::set<std::uint32_t> slots;
  for (const VtableEvidence &candidate : evidence) {
    if (candidate.class_name.empty()) {
      return {};
    }
    classes.insert(candidate.class_name);
    slots.insert(candidate.slot);
  }

  if (classes.size() != 1) {
    // Multiple classes share this implementation. Try to resolve via
    // inheritance: if one class is an ancestor of all others, it is the
    // most likely owner of the virtual function.
    if (inheritance != nullptr && !inheritance->empty()) {
      const std::optional<std::string> ancestor =
          inheritance->findCommonAncestor(classes);
      if (ancestor.has_value()) {
        // Use the slot from the evidence whose class matches the ancestor.
        // If no evidence matches the ancestor directly, fall back to the
        // common slot if all candidates agree.
        std::set<std::uint32_t> ancestor_slots;
        for (const VtableEvidence &candidate : evidence) {
          if (candidate.class_name == *ancestor) {
            ancestor_slots.insert(candidate.slot);
          }
        }
        if (ancestor_slots.size() == 1) {
          const std::uint32_t ancestor_slot = *ancestor_slots.begin();
          bool all_slots_match = true;
          for (const VtableEvidence &candidate : evidence) {
            if (candidate.slot != ancestor_slot) {
              all_slots_match = false;
              break;
            }
          }
          if (all_slots_match) {
            return formatEvidenceLabel(EvidenceSource::Vtable,
                                       *ancestor + "::vfn[" +
                                           std::to_string(ancestor_slot) +
                                           "]");
          }
        }
        if (!ancestor_slots.empty()) {
          return formatEvidenceLabel(EvidenceSource::Vtable,
                                     *ancestor + "::<virtual>", true);
        }
        if (slots.size() == 1) {
          return formatEvidenceLabel(EvidenceSource::Vtable,
                                     *ancestor + "::vfn[" +
                                         std::to_string(*slots.begin()) +
                                         "]");
        }
        return formatEvidenceLabel(EvidenceSource::Vtable,
                                   *ancestor + "::<virtual>", true);
      }
    }
    return {};
  }

  const std::string &class_name = *classes.begin();
  if (slots.size() == 1) {
    return formatEvidenceLabel(EvidenceSource::Vtable,
                               class_name + "::vfn[" +
                                   std::to_string(*slots.begin()) + "]");
  }
  return formatEvidenceLabel(EvidenceSource::Vtable, class_name + "::<virtual>",
                             true);
}

int scoreStringHint(std::string_view value) {
  if (value.size() < 6 || value.size() > 160) {
    return std::numeric_limits<int>::min();
  }
  const std::string folded = lower(value);
  int score = 8;

  std::size_t letters = 0;
  std::size_t words = 0;
  bool in_word = false;
  for (unsigned char c : value) {
    if (std::isalpha(c)) {
      ++letters;
      if (!in_word) {
        ++words;
        in_word = true;
      }
    } else {
      in_word = false;
    }
  }
  if (letters * 2 < value.size() || words < 2) {
    score -= 18;
  } else {
    score += static_cast<int>(std::min<std::size_t>(words, 6)) * 2;
  }

  if (folded.find(" - tick") != std::string::npos) {
    score += 90;
  }
  if (folded.ends_with(" update") ||
      folded.find(" - update") != std::string::npos) {
    score += 62;
  }
  if (folded.find("run ") != std::string::npos || folded.starts_with("run ")) {
    score += 28;
  }
  if (folded.find(" task") != std::string::npos) {
    score += 20;
  }
  if (folded.find("coroutine") != std::string::npos) {
    score += 28;
  }
  if (containsAny(folded,
                  {"level", "server", "minecraft", "chunk", "entity", "actor",
                   "player", "network", "raknet", "storage", "script",
                   "redstone", "pathfinding", "navigation", "worker"})) {
    score += 18;
  }

  if (containsAny(folded, {".cpp", ".h:", "\\src\\", "/src/", "http://",
                           "https://", "uuid"})) {
    score -= 100;
  }
  if (folded.starts_with("t *") ||
      folded.find("nonownerpointer<") != std::string::npos ||
      (folded.find('<') != std::string::npos &&
       folded.find('>') != std::string::npos)) {
    score -= 100;
  }
  if (value.find('%') != std::string_view::npos ||
      value.find("{}") != std::string_view::npos) {
    score -= 32;
  }
  if (containsAny(folded, {"assert", "failed", "failure", "error", "unable",
                           "timeout", "violation", "overwritten", "exceed",
                           "dangling", "saved during", "not found"})) {
    score -= 48;
  }
  if (value.find("::") != std::string_view::npos) {
    score -= 12;
  }
  if (value.size() > 64) {
    score -= 28;
  }
  if (value.size() <= 16 && value.back() == ':') {
    score -= 48;
  }
  return score;
}

std::string formatStringHint(std::string_view value, int score) {
  if (score < kMinimumStringHintScore) {
    return {};
  }
  constexpr std::size_t kMaximum = 52;
  std::string message(value.substr(0, kMaximum));
  if (value.size() > kMaximum) {
    message.resize(kMaximum - 3);
    message += "...";
  }
  return formatEvidenceLabel(EvidenceSource::String, message,
                             score < kStrongStringHintScore);
}

} // namespace spark::symbol_guess
