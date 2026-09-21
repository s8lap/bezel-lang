// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #4517 (§18.5, spec v0.306): an expression that is invalid INSIDE a
// string-interpolation hole was reported at 1:1 — the first byte of the
// file — instead of at the hole. Root cause: a hole's `{...}` content is
// re-lexed from a standalone heap copy of just that text (Parser::
// splitInterpolation, src/parser/Parser.cpp), and that sub-Lexer always
// started its own line/column count at {1, 1, 0}, so ANY diagnostic raised
// while parsing the hole's expression carried a location relative to the
// hole's own text rather than the enclosing file.
//
// Here the hole is `{if cmp < 0 then 1 else (if cmp = 0 then 2 else 3)}`:
// an if-expression used as a *value* needs its own outer parens (the valid
// spelling is `{(if cmp < 0 then ...)}`), so "expected expression" is the
// CORRECT verdict — only the location was wrong. This pins the location,
// not the rejection: without the fix, the diagnostic below reads
// `interp_hole_loc_4517__if_expr_reports_at_hole.bzl:1:1: expected
// expression`; with it, it points at the hole's `if`, line 5 column 31
// (right after the opening `{`, matching where the sub-parse's very first
// token — `if` — is rejected by primary-expression parsing).
//
// Fix: Lexer gained a `base` constructor overload (src/lexer/Lexer.hpp /
// .cpp) that rebases every location `locationAt` produces onto a given
// starting SourceLocation instead of always {1, 1, 0}; splitInterpolation
// now computes the hole expression's real file location (walking the
// enclosing token's own text from its already-known `span.begin`) and
// passes it as that base when constructing the sub-Lexer.

module M;
class C
  public static method Main(): int
    var cmp := 0;
    Console.WriteLine($"sign={if cmp < 0 then 1 else (if cmp = 0 then 2 else 3)}");
    // The whole point of #4517 is the LOCATION, so both halves are pinned
    // exactly. `[[@LINE-7]]` is relative to this CHECK, so editing the header
    // comment above cannot silently re-point it; column 31 is the `if` that
    // opens the hole, counted from the start of the line. A `{{[0-9]+}}`
    // wildcard (the idiom the sibling hole test uses) would match the pre-fix
    // `1:1` too and pin nothing.
    // CHECK: interp_hole_loc_4517__if_expr_reports_at_hole.bzl:[[@LINE-7]]:31: expected expression
    return 0;
  end Main;
end C;
