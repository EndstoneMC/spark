#include "sampler/symbol_guess_evidence.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #expr     \
                << '\n';                                                       \
      ++failures;                                                              \
    }                                                                          \
  } while (false)

} // namespace

int main() {
  using spark::symbol_guess::EvidenceSource;
  using spark::symbol_guess::VtableEvidence;

  CHECK(spark::symbol_guess::formatEvidenceLabel(
            EvidenceSource::Rtti, "ServerLevel") == "rtti: ServerLevel");
  CHECK(spark::symbol_guess::formatEvidenceLabel(EvidenceSource::String,
                                                 "chunk loading", true) ==
        "str?: chunk loading");
  CHECK(spark::symbol_guess::formatEvidenceLabel(EvidenceSource::Vtable, "") ==
        "");

  CHECK(spark::symbol_guess::chooseVtableLabel({{"Widget", 3, false, false}}) ==
        "vtable: Widget::vfn[3]");
  CHECK(spark::symbol_guess::chooseVtableLabel({{"Widget", 3, true, false},
                                                {"Widget", 3, false, true},
                                                {"Widget", 3, false, true}}) ==
        "vtable: Widget::vfn[3]");
  CHECK(spark::symbol_guess::chooseVtableLabel(
            {{"Widget", 1, false, false}, {"Widget", 4, false, false}}) ==
        "vtable?: Widget::<virtual>");
  CHECK(spark::symbol_guess::chooseVtableLabel(
            {{"Widget", 3, false, false}, {"Gadget", 3, false, false}})
            .empty());

  const int strong =
      spark::symbol_guess::scoreStringHint("Level - tick redstone");
  const int tentative = spark::symbol_guess::scoreStringHint("Run one task");
  CHECK(strong >= spark::symbol_guess::kStrongStringHintScore);
  CHECK(tentative >= spark::symbol_guess::kMinimumStringHintScore);
  CHECK(tentative < spark::symbol_guess::kStrongStringHintScore);
  CHECK(spark::symbol_guess::formatStringHint(
            "Level - tick redstone", strong) == "str: Level - tick redstone");
  CHECK(spark::symbol_guess::formatStringHint("Run one task", tentative) ==
        "str?: Run one task");
  CHECK(spark::symbol_guess::formatStringHint(
            "Name: ", spark::symbol_guess::scoreStringHint("Name: "))
            .empty());
  CHECK(spark::symbol_guess::formatStringHint("Level - tick redstone", strong)
            .find('%') == std::string::npos);

  if (failures != 0) {
    std::cerr << failures << " evidence test(s) failed\n";
    return 1;
  }
  std::cout << "symbol guess evidence tests passed\n";
  return 0;
}
