#include "core/fx/fx_condition.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace dayo::core::fx {
namespace {

std::string trimCopy(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

std::string toLowerCopy(std::string_view value) {
    std::string out(value);
    for (auto& ch : out)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

// Normalizes event names: lowercase, drops '_', '-', ' '.
std::string eventKey(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (char ch : toLowerCopy(trimCopy(name))) {
        if (ch == '_' || ch == '-' || ch == ' ')
            continue;
        out += ch;
    }
    return out;
}

bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs) {
    return toLowerCopy(lhs) == toLowerCopy(rhs);
}

std::vector<std::string> splitEvents(std::string_view text) {
    std::vector<std::string> parts;
    std::string current;
    for (char ch : text) {
        if (ch == ',' || ch == '+' || ch == '|' || ch == ';') {
            parts.push_back(trimCopy(current));
            current.clear();
        } else {
            current += ch;
        }
    }
    parts.push_back(trimCopy(current));
    // Also split on whitespace runs that separate bare event names when no
    // delimiter was used (e.g. "load frame"). Only split if every token is a
    // known event so predicates with spaces stay intact.
    if (parts.size() == 1) {
        std::vector<std::string> words;
        std::string word;
        for (char ch : parts.front()) {
            if (ch == ' ' || ch == '\t') {
                if (!word.empty()) {
                    words.push_back(word);
                    word.clear();
                }
            } else {
                word += ch;
            }
        }
        if (!word.empty())
            words.push_back(word);
        if (words.size() > 1) {
            bool allKnown = true;
            for (const auto& candidate : words) {
                if (fxEventFromName(candidate) == kFxEventNone) {
                    allKnown = false;
                    break;
                }
            }
            if (allKnown)
                return words;
        }
    }
    std::vector<std::string> out;
    out.reserve(parts.size());
    for (auto& part : parts) {
        if (!part.empty())
            out.push_back(std::move(part));
    }
    return out;
}

// Splits "events [if|when|:|where] predicate" at the first predicate marker.
void splitPredicateMarker(const std::string& text, std::string& events, std::string& predicate) {
    const std::string lowered = toLowerCopy(text);
    const auto startsWithWord = [&](std::string_view word) {
        if (!lowered.starts_with(word))
            return false;
        return lowered.size() == word.size() || std::isspace(static_cast<unsigned char>(lowered[word.size()])) != 0 ||
               lowered[word.size()] == '(';
    };
    // Predicate-only forms have no event prefix. Checking only for a
    // whitespace-surrounded marker misses the common source form
    // "if time > 0".
    for (const std::string_view marker : {std::string_view{"if"}, std::string_view{"where"}}) {
        if (startsWithWord(marker)) {
            events.clear();
            predicate = trimCopy(std::string_view(text).substr(marker.size()));
            return;
        }
    }
    // `when` is also an event-prefix alias, so only its parenthesized form is
    // unambiguously predicate-only (`when (FRAME > 0)`).
    if (startsWithWord("when")) {
        const auto remainder = trimCopy(std::string_view(text).substr(4));
        if (!remainder.empty() && remainder.front() == '(') {
            events.clear();
            predicate = remainder;
            return;
        }
    }
    // Colon form: "when resize: w > 0" or "frame: time>0".
    const auto colon = text.find(':');
    // Avoid treating "::" scope as a marker.
    if (colon != std::string::npos && text.find("::") == std::string::npos) {
        events = trimCopy(std::string_view(text).substr(0, colon));
        predicate = trimCopy(std::string_view(text).substr(colon + 1));
        return;
    }
    const std::string_view markers[] = {" if ", " when ", " where "};
    for (const std::string_view marker : markers) {
        const auto pos = lowered.find(marker);
        if (pos != std::string::npos) {
            events = trimCopy(std::string_view(text).substr(0, pos));
            predicate = trimCopy(std::string_view(text).substr(pos + marker.size()));
            return;
        }
    }
    events = trimCopy(text);
    predicate.clear();
}

std::string stripPrefixWord(std::string events) {
    std::string lowered = toLowerCopy(events);
    const std::string_view prefixes[] = {"on ", "when "};
    for (const std::string_view prefix : prefixes) {
        if (lowered.starts_with(prefix))
            return trimCopy(std::string_view(events).substr(prefix.size()));
    }
    if (equalsIgnoreCase(events, "on") || equalsIgnoreCase(events, "when"))
        return {};
    return events;
}

} // namespace

const char* toString(FxEvent event) noexcept {
    switch (event) {
    case FxEvent::load:
        return "load";
    case FxEvent::start:
        return "start";
    case FxEvent::resize:
        return "resize";
    case FxEvent::frame:
        return "frame";
    case FxEvent::modelChanged:
        return "modelChanged";
    case FxEvent::materialChanged:
        return "materialChanged";
    case FxEvent::none:
        return "none";
    }
    return "none";
}

FxEventMask fxEventFromName(std::string_view name) noexcept {
    const std::string key = eventKey(name);
    if (key == "load" || key == "onload")
        return kFxEventLoad;
    if (key == "start" || key == "onstart")
        return kFxEventStart;
    if (key == "resize" || key == "onresize")
        return kFxEventResize;
    if (key == "frame" || key == "onframe")
        return kFxEventFrame;
    if (key == "modelchanged")
        return kFxEventModelChanged;
    if (key == "materialchanged")
        return kFxEventMaterialChanged;
    return kFxEventNone;
}

std::string toStringMask(FxEventMask mask) {
    if (mask == kFxEventNone)
        return "none";
    std::string out;
    const std::pair<FxEventMask, const char*> table[] = {
        {kFxEventLoad, "load"},
        {kFxEventStart, "start"},
        {kFxEventResize, "resize"},
        {kFxEventFrame, "frame"},
        {kFxEventModelChanged, "modelChanged"},
        {kFxEventMaterialChanged, "materialChanged"},
    };
    for (const auto& [bit, name] : table) {
        if ((mask & bit) != 0U) {
            if (!out.empty())
                out += ",";
            out += name;
        }
    }
    return out.empty() ? "none" : out;
}

FxConditionProgram compileFxCondition(std::string_view source) {
    FxConditionProgram program;
    const std::string text = trimCopy(source);
    if (text.empty()) {
        log::debug("fx condition: empty source compiles to unconditional");
        return program;
    }
    std::string eventsText;
    std::string predicateText;
    splitPredicateMarker(text, eventsText, predicateText);
    eventsText = stripPrefixWord(eventsText);
    if (eventsText.empty()) {
        // Predicate-only form such as "if time > 0": unconditional events.
        program.predicate = predicateText;
        log::debug("fx condition: predicate-only '", program.predicate, "'");
        return program;
    }
    for (const auto& token : splitEvents(eventsText)) {
        const FxEventMask bit = fxEventFromName(token);
        if (bit == kFxEventNone) {
            log::error("fx condition: unknown event '", token, "' in '", std::string(source), "'");
            throw std::runtime_error("unknown fx event: " + token);
        }
        program.events |= bit;
        program.eventNames.emplace_back(token);
    }
    program.predicate = predicateText;
    log::debug("fx condition '", std::string(source), "' -> events=", toStringMask(program.events), " predicate='",
               program.predicate, "'");
    return program;
}

bool fxConditionMatches(const FxConditionProgram& program, FxEventMask active) noexcept {
    if (program.events == kFxEventNone)
        return true;
    return (program.events & active) != 0U;
}

void FxConditionScheduler::add(FxConditionProgram condition, int passIndex, std::string passName) {
    log::debug("fx scheduler: add pass ", passIndex, " '", passName, "' events=", toStringMask(condition.events));
    passes_.push_back(FxScheduledPass{
        .condition = std::move(condition),
        .passIndex = passIndex,
        .passName = std::move(passName),
    });
    std::sort(passes_.begin(), passes_.end(),
              [](const FxScheduledPass& lhs, const FxScheduledPass& rhs) { return lhs.passIndex < rhs.passIndex; });
}

void FxConditionScheduler::clear() noexcept {
    passes_.clear();
}

std::vector<int> FxConditionScheduler::activePasses(FxEventMask active) const {
    std::vector<int> out;
    out.reserve(passes_.size());
    for (const auto& pass : passes_) {
        if (fxConditionMatches(pass.condition, active))
            out.push_back(pass.passIndex);
    }
    return out;
}

} // namespace dayo::core::fx
