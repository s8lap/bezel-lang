#pragma once

// Private implementation header for the parser (mirrors the convention set by
// LlvmBackend_impl.hpp / ExprTypecheck_impl.hpp / TypeResolution_impl.hpp for
// a class split across several translation units): free helpers shared by the
// Parser.cpp / Parser_Decls.cpp / Parser_Exprs.cpp / Parser_Stmts.cpp split
// that are not part of the Parser class's public surface (Parser.hpp) live
// here, `inline`, so there is exactly ONE definition even though several TUs
// need it. Included ONLY by those implementation files.

#include <string_view>

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

} // namespace bezel
