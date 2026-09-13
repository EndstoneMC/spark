#include "native/symbol/symbol_guess_evidence.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>

namespace spark::symbol_guess {

namespace {

GuessKind guessKindFromSource(EvidenceSource source)
{
    switch (source) {
    case EvidenceSource::Rtti:
        return GuessKind::Rtti;
    case EvidenceSource::String:
        return GuessKind::String;
    case EvidenceSource::Vtable:
        return GuessKind::Vtable;
    case EvidenceSource::Thunk:
        return GuessKind::Thunk;
    }
    return GuessKind::None;
}

std::string lower(std::string_view value)
{
    std::string out(value);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool containsAny(std::string_view value, std::initializer_list<std::string_view> needles)
{
    return std::ranges::any_of(
        needles, [value](std::string_view needle) { return value.find(needle) != std::string_view::npos; });
}

std::string_view sourceName(EvidenceSource source)
{
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

}  // namespace

void InheritanceMap::addBase(std::string_view derived, std::string_view base)
{
    parents_[std::string(derived)].insert(std::string(base));
}

bool InheritanceMap::isAncestor(std::string_view ancestor, std::string_view descendant) const
{
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

std::optional<std::string> InheritanceMap::findCommonAncestor(const std::set<std::string> &classes) const
{
    if (classes.empty()) {
        return std::nullopt;
    }
    // Only an observed class may own a shared implementation. An unobserved
    // common base is useful for hierarchy discovery but is not evidence that
    // this particular vtable belongs to that base.
    std::optional<std::string> owner;
    for (const std::string &candidate : classes) {
        bool ancestor_of_all = true;
        for (const std::string &other : classes) {
            if (candidate != other && !isAncestor(candidate, other)) {
                ancestor_of_all = false;
                break;
            }
        }
        if (!ancestor_of_all) {
            continue;
        }
        if (owner.has_value()) {
            return std::nullopt;
        }
        owner = candidate;
    }
    return owner;
}

TypedLabel formatEvidenceLabel(EvidenceSource source, std::string_view message, bool tentative)
{
    if (message.empty()) {
        return {};
    }
    std::string out(sourceName(source));
    if (tentative) {
        out += '?';
    }
    out += ": ";
    out += message;
    return {.label = std::move(out),
            .kind = guessKindFromSource(source),
            .confidence = tentative ? Confidence::Medium : Confidence::High};
}

TypedLabel chooseVtableLabel(std::vector<VtableEvidence> evidence, const InheritanceMap *inheritance)
{
    std::ranges::sort(evidence, [](const VtableEvidence &a, const VtableEvidence &b) {
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
    const auto duplicate = std::ranges::unique(evidence);
    evidence.erase(duplicate.begin(), duplicate.end());
    if (evidence.empty()) {
        return {};
    }

    std::set<std::string> classes;
    std::set<std::uint32_t> slots;
    bool has_secondary = false;
    bool has_thunk = false;
    for (const VtableEvidence &candidate : evidence) {
        if (candidate.class_name.empty()) {
            return {};
        }
        classes.insert(candidate.class_name);
        slots.insert(candidate.slot);
        has_secondary = has_secondary || candidate.secondary;
        has_thunk = has_thunk || candidate.via_thunk;
    }

    std::optional<std::string> owner;
    if (classes.size() == 1) {
        owner = *classes.begin();
    }
    else if (inheritance != nullptr && !inheritance->empty()) {
        owner = inheritance->findCommonAncestor(classes);
    }
    if (!owner.has_value()) {
        return {};
    }

    const bool strong = slots.size() == 1 && !has_secondary && !has_thunk;
    if (strong) {
        return formatEvidenceLabel(EvidenceSource::Vtable, *owner + "::vfn[" + std::to_string(*slots.begin()) + "]");
    }
    return formatEvidenceLabel(EvidenceSource::Vtable, *owner + "::<virtual>", true);
}

int scoreStringHint(std::string_view value)
{
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
        }
        else {
            in_word = false;
        }
    }
    if (letters * 2 < value.size() || words < 2) {
        score -= 18;
    }
    else {
        score += static_cast<int>(std::min<std::size_t>(words, 6)) * 2;
    }

    if (folded.find(" - tick") != std::string::npos) {
        score += 90;
    }
    if (folded.ends_with(" update") || folded.find(" - update") != std::string::npos) {
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
    if (containsAny(folded, {"level", "server", "minecraft", "chunk", "entity", "actor", "player", "network", "raknet",
                             "storage", "script", "redstone", "pathfinding", "navigation", "worker"})) {
        score += 18;
    }

    // BDS trace strings: "N _functionName" or "N functionName" where N is 1-2
    // digits. These are debug trace markers loaded via lea and uniquely tied
    // to their containing function.
    if (std::isdigit(static_cast<unsigned char>(value[0]))) {
        const std::size_t sp = value.find(' ');
        if (sp <= 2 && sp + 1 < value.size()) {
            const std::string_view rest = value.substr(sp + 1);
            if (rest.size() >= 4) {
                bool has_alpha = false;
                bool clean = true;
                for (unsigned char c : rest) {
                    if (!std::isalnum(c) && c != '_') {
                        clean = false;
                        break;
                    }
                    if (std::isalpha(c)) {
                        has_alpha = true;
                    }
                }
                if (clean && has_alpha) {
                    score += 45;
                }
            }
        }
    }

    if (containsAny(folded, {".cpp", ".h:", "\\src\\", "/src/", "http://", "https://", "uuid"})) {
        score -= 100;
    }
    if (folded.starts_with("t *") || folded.find("nonownerpointer<") != std::string::npos ||
        (folded.find('<') != std::string::npos && folded.find('>') != std::string::npos)) {
        score -= 100;
    }
    if (value.find('%') != std::string_view::npos || value.find("{}") != std::string_view::npos) {
        score -= 32;
    }
    if (containsAny(folded, {"assert", "failed", "failure", "error", "unable", "timeout", "violation", "overwritten",
                             "exceed", "dangling", "saved during", "not found"})) {
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

TypedLabel formatStringHint(std::string_view value, int score)
{
    if (score < kMinimumStringHintScore) {
        return {};
    }
    constexpr std::size_t k_maximum = 52;
    std::string message(value.substr(0, k_maximum));
    if (value.size() > k_maximum) {
        message.resize(k_maximum - 3);
        message += "...";
    }
    return formatEvidenceLabel(EvidenceSource::String, message, score < kStrongStringHintScore);
}

}  // namespace spark::symbol_guess
