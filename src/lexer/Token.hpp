#pragma once

// Tokens produced by the lexer (§1 Lexical Structure). The full token taxonomy
// — keywords (Appendix B), operators and punctuation (§17), and literal forms
// (§18) — is defined by the lexer milestone. This stub carries only the minimum
// the skeleton needs to type-check; the kinds below are NOT the complete set.

#include <string_view>

#include "support/SourceLocation.hpp"

namespace bezel {

enum class TokenKind {
    EndOfFile,
    Identifier,
    Keyword,
    // Operators and punctuation share a single Punctuator kind; the specific
    // symbol is carried in the token lexeme (`text`). Matches the keyword
    // model — per-symbol enum is deferred until the parser needs faster
    // dispatch.
    Punctuator,
    // Numeric literal kinds split int vs float to match the grammar's A.1
    // distinction. Hex/binary integers carry their `0x`/`0b` prefix in the
    // lexeme; decimal floats are FloatLiteral with an `m`/`M` suffix. Value
    // parsing and range-check (e.g. §18.1's "3_000_000_000 without L is a
    // compile error") are sema-level concerns.
    IntLiteral,
    FloatLiteral,
    CharLiteral,
    StringLiteral,
    VerbatimStringLiteral,
    // Interpolated-string kinds (§18.5 / §18.7). Slice 51 lexes the entire
    // `$"..."` (or `$@"..."`) lexeme as a single token; full
    // fragment+expression splitting waits on a follow-up slice that wires
    // the lexer's interp-mode dance to the parser. The InterpolatedString
    // token's `text` includes the surrounding `$"` … `"` quotes; sema
    // (and a future split-pass) decode the `{` … `}` interpolation parts.
    InterpolatedString,
    InterpolatedVerbatimString,
    // per §1.3 / §17.13 / A.1 / B.1 (v0.244, proposal 0083): the bare
    // single-character `_`. A.1 splits the Identifier production —
    // `Identifier ::= Letter IdentCont* | "_" IdentCont+` — so the bare token
    // is NOT an Identifier at all; it is held out of the identifier namespace
    // like a punctuation symbol (B.1 "The reserved token `_`"), never
    // declarable and never readable as a name. Identifiers that merely BEGIN
    // with or contain `_` (`_camelCase`, `_f`, `__`, `_1`, `snake_case`) stay
    // ordinary Identifiers — only the one-character lexeme is this kind.
    Discard,
    Unknown,
};

struct Token {
    TokenKind kind = TokenKind::EndOfFile;
    std::string_view text;
    SourceLocation location;
};

} // namespace bezel
