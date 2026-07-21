// Regression tests for issue #2090 — reserved-keyword-as-identifier
// diagnostics, derived from Appendix B.1 (v0.215).

#include <string>
#include <string_view>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;

namespace {

// Appendix B.1 (v0.215), verbatim and in specification order. Contextual
// keywords are intentionally absent because they remain legal identifiers
// outside their reserved contexts.
constexpr std::string_view kUnconditionallyReservedKeywords[] = {
    "abstract", "and", "as", "async", "await", "base", "bool", "break",
    "byte", "case", "catch", "char", "choice", "class", "const",
    "constructor", "continue", "decimal", "default", "destructor", "do",
    "double", "elif", "else", "end", "enum", "extension", "extern",
    "false", "finally", "float", "for", "if", "import", "in", "int",
    "interface", "internal", "is", "long", "method", "mod", "module",
    "move", "nameof", "new", "not", "null", "or", "out", "owned",
    "override", "private", "property", "protected", "public", "readonly",
    "record", "ref", "return", "sbyte", "sealed", "short", "static",
    "string", "then", "this", "throw", "true", "try", "type", "typeof",
    "uint", "ulong", "ushort", "using", "var", "virtual", "when", "where",
    "while", "with", "yield",
};

} // namespace

// Appendix B.1 (v0.215) / #2090: every unconditionally reserved keyword used
// as a local-variable identifier receives one direct, actionable diagnostic.
// In particular, `var out: int := 0;` must not cascade into opaque
// "expected name/expression/semicolon" errors.
BEZEL_TEST(parser_reserved_keyword_diagnostic_2090,
           spec_B_1_each_reserved_keyword_as_local_name_is_explicit) {
    for (std::string_view keyword : kUnconditionallyReservedKeywords) {
        std::string source =
            "module M; class C public method F() var ";
        source += keyword;
        source += ": int := 0; end F; end C;";

        auto r = parseAll(source);
        BEZEL_EXPECT_EQ(r.diagnostics.errorCount(), std::size_t{1});
        if (r.diagnostics.diagnostics().size() != 1) continue;

        std::string expected = "`";
        expected += keyword;
        expected +=
            "` is a reserved keyword and cannot be used as an identifier "
            "(Appendix B.1); rename it";
        BEZEL_EXPECT_EQ(r.diagnostics.diagnostics()[0].message, expected);
    }
}
