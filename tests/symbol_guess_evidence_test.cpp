#include "sampler/symbol_guess_evidence.h"
#include "sampler/symbolicate.h"

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

  spark::ResolvedFrame normalized;
  spark::GuessResult range_only;
  range_only.function_rva = 0x1200;
  spark::applySymbolGuessFallback(normalized, 0x1234, true, range_only);
  CHECK(normalized.method_name == "0x1200");

  spark::ResolvedFrame guessed;
  spark::GuessResult detailed;
  detailed.function_rva = 0x1200;
  detailed.label = "vtable: Level::vfn[3]";
  detailed.kind = spark::GuessKind::Vtable;
  detailed.confidence = spark::Confidence::High;
  detailed.evidence_count = 1;
  spark::applySymbolGuessFallback(guessed, 0x1234, true, detailed);
  CHECK(guessed.method_name == "0x1200 (vtable: Level::vfn[3])");

  spark::ResolvedFrame resolved;
  resolved.method_name = "Level::tick";
  spark::applySymbolGuessFallback(resolved, 0x1234, true, detailed);
  CHECK(resolved.method_name == "Level::tick");
  CHECK(resolved.guessed_function_rva == 0);

  spark::ResolvedFrame library;
  spark::applySymbolGuessFallback(library, 0x1234, false, detailed);
  CHECK(library.method_name == "0x1234");
  CHECK(library.guessed_function_rva == 0);

  if (failures != 0) {
    std::cerr << failures << " evidence test(s) failed\n";
    return 1;
  }
  std::cout << "symbol guess evidence tests passed\n";
  return 0;
}
