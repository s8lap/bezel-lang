#pragma once

// Private implementation header for the parser (mirrors the convention set by
// LlvmBackend_impl.hpp / ExprTypecheck_impl.hpp / TypeResolution_impl.hpp for
// a class split across several translation units): free helpers shared by the
// Parser.cpp / Parser_Decls.cpp / Parser_Exprs.cpp / Parser_Stmts.cpp split
// that are not part of the Parser class's public surface (Parser.hpp) live
// here, `inline`, so there is exactly ONE definition even though several TUs
// need it. Included ONLY by those implementation files.

#include <string_view>

#include "lexer/Token.hpp"

namespace bezel {

// §3.4: the lowercase primitive-type aliases. These are unconditionally
// reserved keywords (Appendix B.1) but they ARE valid type-name first
// components — `int`, `string`, etc. The PascalCase forms (`Object`, `String`,
// `Array`) are ordinary identifiers and need no special handling here.
//
// fix #3556-family dedup (Run 9): this predicate used to be defined
// separately, byte-identical, in the anonymous namespace of THREE TUs
// (Parser.cpp, Parser_Decls.cpp, Parser_Exprs.cpp) — three copies of one
// decision that would have been free to drift silently. Hoisted here so
// there is one spelling.
inline bool isPrimitiveTypeKeyword(std::string_view text) {
    return text == "bool"   || text == "byte"  || text == "sbyte" ||
           text == "short"  || text == "ushort" || text == "int"  ||
           text == "uint"   || text == "long"  || text == "ulong" ||
           text == "float"  || text == "double" || text == "decimal" ||
           text == "char"   || text == "string";
}

// fix #7163 (§22.12, v0.343): the generics-vs-less-than resolution's
// SUCCESSOR SET, spelled out once.
//
// "After an identifier in expression position, a `<` opens generic arguments
// **iff** scanning forward finds a balanced `<…>` group (respecting nesting)
// whose closing `>` is immediately followed by a token that can continue a
// primary expression — `.`, `?.`, `(`, or `[` — or by a token that cannot
// begin an operand — `)`, `]`, `,`, or `;` (a relational `>` always has a
// right operand, so no relational reading is lost; this is how a method
// reference at explicit type arguments, `xs.Select(Util.Id<int>)` or
// `var k := Util.Keep<Pair>;`, parses as one, §27.6).  Otherwise the `<` is
// the relational operator."
//
// EIGHT successors.  This predicate used to be spelled out — as two
// DIFFERENT subsets, five tokens and one — in two scan helpers of
// Parser_Exprs.cpp, so `?.` and `[` were honoured nowhere and the set could
// drift per site again.  Hoisted here for the same reason
// `isPrimitiveTypeKeyword` was: one spelling, one decision.
inline bool genericArgsCloserFollows(const Token& after) {
    if (after.kind != TokenKind::Punctuator) return false;
    // A token that can continue a primary expression (`.`, `?.`, `(`, `[`) or
    // one that cannot begin an operand (`)`, `]`, `,`, `;`).
    return after.text == "."  || after.text == "?." ||
           after.text == "("  || after.text == "["  ||
           after.text == ")"  || after.text == "]"  ||
           after.text == ","  || after.text == ";";
}

} // namespace bezel
