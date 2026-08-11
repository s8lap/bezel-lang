#pragma once

// Grammar A.8 / §4.2: the ONE parser from a Bezel integer-literal LEXEME to its
// 64-bit value.
//
// It lives here rather than in either consumer because there were two
// definitions and one of them was wrong (#2936). Codegen carried this correct
// one (`parseIntLiteral`, LlvmBackend_impl.hpp) while `sema::foldIntConst`
// (ConstFold.cpp) had its own `std::stoll(text)` — a base-10 parse, which
// silently returns 0 for `0x60` (it reads the leading `0` and stops at `x`,
// so no exception is thrown and no fallback fires) and 5 for `5_0`. The
// grammar admits all of these spellings:
//
//     HexLiteral ::= "0x" HexDigit ("_"* HexDigit)* IntSuffix?
//     BinLiteral ::= "0b" ("0" | "1") ("_"* ("0" | "1"))* IntSuffix?
//
// so a base-10 parse is wrong for three of the four bases and for every
// underscore-separated literal in any base.
//
// Every consumer of a literal's VALUE must call this. The lexer has already
// validated the form; this only decodes it.

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace bezel::common {

// Parse a Bezel integer literal lexeme to its value. Handles decimal and the
// `0x` / `0b` / `0o` bases, strips `_` digit separators, and ignores any
// trailing type-suffix letters. Returns 0 on a shape it does not recognize
// (the lexer already validated the literal).
inline std::int64_t parseIntLiteral(std::string_view text) {
    std::string digits;
    int base = 10;
    std::size_t i = 0;
    if (text.size() >= 2 && text[0] == '0') {
        char b = text[1];
        if (b == 'x' || b == 'X') { base = 16; i = 2; }
        else if (b == 'b' || b == 'B') { base = 2; i = 2; }
        else if (b == 'o' || b == 'O') { base = 8; i = 2; }
    }
    for (; i < text.size(); ++i) {
        char c = text[i];
        if (c == '_') continue;
        // Stop at a type-suffix letter (e.g. `u`, `l`) for decimal; for
        // hex, hex-digits are consumed and only non-hex letters stop us.
        bool isDigit = (c >= '0' && c <= '9');
        bool isHex = isDigit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (base == 16 ? !isHex : !isDigit) break;
        digits.push_back(c);
    }
    if (digits.empty()) return 0;
    // §4.2 / A.1: parse the magnitude with the UNSIGNED strtoull regardless of
    // the trailing IntSuffix. The caller always stores the result as a 64-bit
    // pattern (int64 -> uint64 -> i64 LLVM constant), and `digits` is the bare
    // magnitude — the sign of a negative literal is a separate UnaryExpr — so
    // the full [0, 2^64) range is meaningful. Signed strtoll SATURATES at
    // long.MaxValue (and sets ERANGE) for any magnitude in [2^63, 2^64):
    //   - a `ulong` value above long.MaxValue (e.g. 18000000000000000000UL)
    //     lost its value (issue #421 — already handled for the `U` case), and
    //   - the 2^63 magnitude of `-9223372036854775808L` (long.MinValue)
    //     saturated to 2^63-1, so the literal lowered to INT64_MIN+1 instead of
    //     INT64_MIN (the codegen counterpart of the #749 fold bug).
    // strtoull preserves the bit pattern across the whole range; the
    // out-of-range guard lives in sema (the literal type-check, §5).
    return static_cast<std::int64_t>(
        std::strtoull(digits.c_str(), nullptr, base));
}

}  // namespace bezel::common
