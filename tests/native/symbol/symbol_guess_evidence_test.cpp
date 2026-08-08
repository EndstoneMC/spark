#include <iostream>
#include <string>

#include "native/symbol/symbol_guess_evidence.h"

namespace {

int failures = 0;

#define CHECK(expr)                                                                       \
    do {                                                                                  \
        if (!(expr)) {                                                                    \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #expr << '\n'; \
            ++failures;                                                                   \
        }                                                                                 \
    } while (false)

}  // namespace

int main()
{
    using spark::symbol_guess::EvidenceSource;
    using spark::symbol_guess::VtableEvidence;

    CHECK(spark::symbol_guess::formatEvidenceLabel(EvidenceSource::Rtti, "ServerLevel").label == "rtti: ServerLevel");
    CHECK(spark::symbol_guess::formatEvidenceLabel(EvidenceSource::String, "chunk loading", true).label ==
          "str?: chunk loading");
    CHECK(spark::symbol_guess::formatEvidenceLabel(EvidenceSource::Vtable, "").empty());

    // Typed kind/confidence checks.
    {
        auto rtti = spark::symbol_guess::formatEvidenceLabel(EvidenceSource::Rtti, "ServerLevel");
        CHECK(rtti.kind == spark::GuessKind::Rtti);
        CHECK(rtti.confidence == spark::Confidence::High);

        auto str_tent = spark::symbol_guess::formatEvidenceLabel(EvidenceSource::String, "chunk loading", true);
        CHECK(str_tent.kind == spark::GuessKind::String);
        CHECK(str_tent.confidence == spark::Confidence::Medium);

        auto vtable = spark::symbol_guess::formatEvidenceLabel(EvidenceSource::Vtable, "A::vfn[0]");
        CHECK(vtable.kind == spark::GuessKind::Vtable);
        CHECK(vtable.confidence == spark::Confidence::High);

        auto thunk = spark::symbol_guess::formatEvidenceLabel(EvidenceSource::Thunk, "target", true);
        CHECK(thunk.kind == spark::GuessKind::Thunk);
        CHECK(thunk.confidence == spark::Confidence::Medium);
    }

    CHECK(spark::symbol_guess::chooseVtableLabel({{"Widget", 3, false, false}}).label == "vtable: Widget::vfn[3]");
    CHECK(spark::symbol_guess::chooseVtableLabel(
              {{"Widget", 3, true, false}, {"Widget", 3, false, true}, {"Widget", 3, false, true}})
              .label == "vtable: Widget::vfn[3]");
    CHECK(spark::symbol_guess::chooseVtableLabel({{"Widget", 1, false, false}, {"Widget", 4, false, false}}).label ==
          "vtable?: Widget::<virtual>");
    CHECK(spark::symbol_guess::chooseVtableLabel({{"Widget", 3, false, false}, {"Gadget", 3, false, false}}).empty());

    // --- InheritanceMap tests ---
    using spark::symbol_guess::InheritanceMap;

    // Empty map: no resolution, still returns empty.
    {
        InheritanceMap empty;
        CHECK(empty.empty());
        CHECK(empty.empty());
        CHECK(spark::symbol_guess::chooseVtableLabel({{"Widget", 3, false, false}, {"Gadget", 3, false, false}}, &empty)
                  .empty());
    }

    // Gadget inherits from Widget: same slot resolves to ancestor.
    {
        InheritanceMap inh;
        inh.addBase("Gadget", "Widget");
        CHECK(!inh.empty());
        CHECK(inh.size() == 1);
        CHECK(inh.isAncestor("Widget", "Gadget"));
        CHECK(!inh.isAncestor("Gadget", "Widget"));
        const auto common = inh.findCommonAncestor({"Widget", "Gadget"});
        CHECK(common.has_value());
        CHECK(common == "Widget");

        // Both share slot 3 -> high confidence on ancestor.
        CHECK(spark::symbol_guess::chooseVtableLabel({{"Widget", 3, false, false}, {"Gadget", 3, false, false}}, &inh)
                  .label == "vtable: Widget::vfn[3]");

        // Different slots (Widget=3, Gadget=7) -> tentative, no slot.
        CHECK(spark::symbol_guess::chooseVtableLabel({{"Widget", 3, false, false}, {"Gadget", 7, false, false}}, &inh)
                  .label == "vtable?: Widget::<virtual>");

        // Ancestor not in evidence but all slots agree -> high confidence.
        {
            InheritanceMap inh2;
            inh2.addBase("D1", "Widget");
            inh2.addBase("D2", "Widget");
            CHECK(spark::symbol_guess::chooseVtableLabel({{"D1", 5, false, false}, {"D2", 5, false, false}}, &inh2)
                      .label == "vtable: Widget::vfn[5]");
        }
    }

    // Diamond: D -> B,C ; B,C -> A.  Common ancestor of {B,C} is A.
    {
        InheritanceMap inh;
        inh.addBase("B", "A");
        inh.addBase("C", "A");
        inh.addBase("D", "B");
        inh.addBase("D", "C");
        CHECK(inh.isAncestor("A", "D"));
        CHECK(inh.isAncestor("A", "B"));
        CHECK(inh.isAncestor("A", "C"));
        CHECK(inh.isAncestor("B", "D"));
        CHECK(inh.isAncestor("C", "D"));
        CHECK(!inh.isAncestor("B", "C"));
        CHECK(!inh.isAncestor("D", "A"));
        auto ca = inh.findCommonAncestor({"B", "C"});
        CHECK(ca.has_value());
        CHECK(ca == "A");
        auto ca2 = inh.findCommonAncestor({"B", "C", "D"});
        CHECK(ca2.has_value());
        CHECK(ca2 == "A");
    }

    // Unrelated classes: no common ancestor.
    {
        InheritanceMap inh;
        inh.addBase("Gadget", "Widget");
        auto ca = inh.findCommonAncestor({"Widget", "Unrelated"});
        CHECK(!ca.has_value());
        CHECK(
            spark::symbol_guess::chooseVtableLabel({{"Widget", 3, false, false}, {"Unrelated", 3, false, false}}, &inh)
                .empty());
    }

    const int strong = spark::symbol_guess::scoreStringHint("Level - tick redstone");
    const int tentative = spark::symbol_guess::scoreStringHint("Run one task");
    CHECK(strong >= spark::symbol_guess::kStrongStringHintScore);
    CHECK(tentative >= spark::symbol_guess::kMinimumStringHintScore);
    CHECK(tentative < spark::symbol_guess::kStrongStringHintScore);
    CHECK(spark::symbol_guess::formatStringHint("Level - tick redstone", strong).label == "str: Level - tick redstone");
    CHECK(spark::symbol_guess::formatStringHint("Run one task", tentative).label == "str?: Run one task");
    CHECK(spark::symbol_guess::formatStringHint("Name: ", spark::symbol_guess::scoreStringHint("Name: ")).empty());
    CHECK(spark::symbol_guess::formatStringHint("Level - tick redstone", strong).label.find('%') == std::string::npos);

    // BDS trace strings: "N _functionName" pattern.
    const int trace = spark::symbol_guess::scoreStringHint("2 _deserializeEntity");
    CHECK(trace >= spark::symbol_guess::kMinimumStringHintScore);
    CHECK(trace < spark::symbol_guess::kStrongStringHintScore);
    CHECK(spark::symbol_guess::formatStringHint("2 _deserializeEntity", trace).label == "str?: 2 _deserializeEntity");

    // Short trace without domain keywords stays below threshold.
    CHECK(spark::symbol_guess::scoreStringHint("1 _tick") < spark::symbol_guess::kMinimumStringHintScore);

    // Trace string with error keyword is rejected.
    CHECK(spark::symbol_guess::scoreStringHint("3 _assertFailed") < spark::symbol_guess::kMinimumStringHintScore);

    // Non-trace string starting with digit is not boosted.
    CHECK(spark::symbol_guess::scoreStringHint("404 Not Found") < spark::symbol_guess::kMinimumStringHintScore);

    if (failures != 0) {
        std::cerr << failures << " evidence test(s) failed\n";
        return 1;
    }
    std::cout << "symbol guess evidence tests passed\n";
    return 0;
}
