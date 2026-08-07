#ifndef ENDSTONE_SPARK_SYMBOL_GUESS_EVIDENCE_H
#define ENDSTONE_SPARK_SYMBOL_GUESS_EVIDENCE_H

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spark::symbol_guess {

enum class EvidenceSource {
  Rtti,
  String,
  Vtable,
  Thunk,
};

struct VtableEvidence {
  std::string class_name;
  std::uint32_t slot = 0;
  bool secondary = false;
  bool via_thunk = false;

  bool operator==(const VtableEvidence &) const = default;
};

// Maps direct inheritance relationships so that shared vtable implementations
// can be attributed to their common ancestor. The map stores only direct
// parent links; ancestor queries compute the transitive closure with cycle
// detection.
class InheritanceMap {
public:
  void addBase(std::string_view derived, std::string_view base);
  bool isAncestor(std::string_view ancestor,
                  std::string_view descendant) const;
  // Returns the most-derived class that is an ancestor of every candidate,
  // or nullopt when no unique common ancestor exists.
  std::optional<std::string>
  findCommonAncestor(const std::set<std::string> &classes) const;
  bool empty() const { return parents_.empty(); }
  std::size_t size() const { return parents_.size(); }

private:
  std::unordered_map<std::string, std::unordered_set<std::string>> parents_;
};

// Guesses always carry their evidence source. A question mark means the
// evidence is useful but cannot identify the exact member (for example, one
// class mapping the same implementation to several vtable slots). Conflicting
// evidence returns an empty label instead of hiding the conflict behind '?'.
std::string formatEvidenceLabel(EvidenceSource source, std::string_view message,
                                bool tentative = false);
std::string chooseVtableLabel(std::vector<VtableEvidence> evidence,
                              const InheritanceMap *inheritance = nullptr);

// Deterministic semantic scoring shared by both native backends. Scores are an
// internal ranking, not probabilities: accepted strings below the strong
// threshold are displayed as `str?:`, never as a confidence percentage.
int scoreStringHint(std::string_view value);
std::string formatStringHint(std::string_view value, int score);

inline constexpr int kMinimumStringHintScore = 50;
inline constexpr int kStrongStringHintScore = 80;

} // namespace spark::symbol_guess

#endif // ENDSTONE_SPARK_SYMBOL_GUESS_EVIDENCE_H
