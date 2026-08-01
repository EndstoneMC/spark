#ifndef ENDSTONE_SPARK_SYMBOL_GUESS_EVIDENCE_H
#define ENDSTONE_SPARK_SYMBOL_GUESS_EVIDENCE_H

#include <cstdint>
#include <string>
#include <string_view>
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

// Guesses always carry their evidence source. A question mark means the
// evidence is useful but cannot identify the exact member (for example, one
// class mapping the same implementation to several vtable slots). Conflicting
// evidence returns an empty label instead of hiding the conflict behind '?'.
std::string formatEvidenceLabel(EvidenceSource source, std::string_view message,
                                bool tentative = false);
std::string chooseVtableLabel(std::vector<VtableEvidence> evidence);

// Deterministic semantic scoring shared by both native backends. Scores are an
// internal ranking, not probabilities: accepted strings below the strong
// threshold are displayed as `str?:`, never as a confidence percentage.
int scoreStringHint(std::string_view value);
std::string formatStringHint(std::string_view value, int score);

inline constexpr int kMinimumStringHintScore = 50;
inline constexpr int kStrongStringHintScore = 80;

} // namespace spark::symbol_guess

#endif // ENDSTONE_SPARK_SYMBOL_GUESS_EVIDENCE_H
