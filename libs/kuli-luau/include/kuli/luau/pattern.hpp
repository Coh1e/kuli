#pragma once
// Lua string-pattern matching, backed by Luau's own (authoritative) matcher.
// The blueprint DSL's `assetPattern` (design §8.2.5) is a Lua pattern; reusing
// the embedded engine avoids a hand-rolled matcher and keeps semantics exact.
#include <string_view>

namespace kuli::luau {

// True iff `pattern` (a Lua string pattern) matches anywhere in `text`.
// A malformed pattern (Luau raises) yields false.
bool pattern_matches(std::string_view text, std::string_view pattern);

}  // namespace kuli::luau
