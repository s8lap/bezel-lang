#pragma once

// Source positions used by every later phase. Not itself a compilation phase;
// this is shared infrastructure the lexer attaches to tokens (§1 defines source
// text as a sequence of characters) and that diagnostics point at.

#include <cstdint>

namespace bezel {

// A single position in a source file. `line` and `column` are 1-based to match
// how the spec and diagnostics refer to source locations; `offset` is a 0-based
// index into the source buffer.
struct SourceLocation {
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::uint32_t offset = 0;
};

// A half-open [begin, end) range of source text.
struct SourceSpan {
    SourceLocation begin;
    SourceLocation end;
};

} // namespace bezel
