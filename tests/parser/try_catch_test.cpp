// Spec-derived parser tests for TryStmt — §A.6.
//
// Slice 22 covers the binding catch pattern only (`catch var x: Type =>
// stmt`), optional finally, and the two grammar shapes (`try … catch …
// end;` and `try … catch … finally … end;`). The no-binding pattern
// (`catch Type =>`), multi-Type patterns (`Type1, Type2`), `when` guards,
// and Expression / `do … end` arm bodies are deferred to the pattern-
// matching slice (they share grammar with §19 case).

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

static std::string parseBodyText(const std::string& body) {
    auto r = parseAll(std::string("module M; class Foo public method run() ") +
                      body + std::string(" end run; end Foo;"));
    if (r.diagnostics.hasErrors()) return "<error>";
    const auto ms = memberSummaries(r);
    if (ms.size() != 1) return "<no member>";
    const std::string prefix = "public method run() ";
    if (ms[0].rfind(prefix, 0) != 0) return "<bad prefix>";
    return ms[0].substr(prefix.size());
}

BEZEL_TEST(parser_try, simple_try_catch) {
    BEZEL_EXPECT_EQ(
        parseBodyText("try risky(); catch var e: Error => log(e); end;"),
        std::string(
            "{ try { risky(); } catch var e: Error => log(e); end; }"));
}

BEZEL_TEST(parser_try, try_with_multiple_body_statements) {
    BEZEL_EXPECT_EQ(
        parseBodyText(
            "try open(); use(); close(); catch var e: Error => log(e); end;"),
        std::string(
            "{ try { open(); use(); close(); } catch var e: Error => "
            "log(e); end; }"));
}

BEZEL_TEST(parser_try, multiple_catch_arms) {
    BEZEL_EXPECT_EQ(
        parseBodyText(
            "try risky(); "
            "catch var e: IOException => log(e);, "
            "catch var e: SysException => abort(); "
            "end;"),
        std::string(
            "{ try { risky(); } catch var e: IOException => log(e); "
            "catch var e: SysException => abort(); end; }"));
}

BEZEL_TEST(parser_try, try_finally_only) {
    // `try Statement* FinallyClause "end" ";"` (second alternative).
    BEZEL_EXPECT_EQ(
        parseBodyText("try work(); finally cleanup(); end;"),
        std::string(
            "{ try { work(); } finally { cleanup(); } end; }"));
}

BEZEL_TEST(parser_try, try_catch_finally) {
    BEZEL_EXPECT_EQ(
        parseBodyText(
            "try work(); catch var e: Error => log(e); finally cleanup(); "
            "end;"),
        std::string(
            "{ try { work(); } catch var e: Error => log(e); "
            "finally { cleanup(); } end; }"));
}

BEZEL_TEST(parser_try, try_with_qualified_exception_type) {
    BEZEL_EXPECT_EQ(
        parseBodyText(
            "try foo(); catch var e: System.IOException => log(e); end;"),
        std::string(
            "{ try { foo(); } catch var e: System.IOException => "
            "log(e); end; }"));
}

// --- Errors -----------------------------------------------------------------

BEZEL_TEST(parser_try, missing_end_is_error) {
    auto r = parseAll(
        "module M; class Foo public method run() "
        "try f(); catch var e: Error => log(e); end run; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_try, catch_without_arrow_is_error) {
    auto r = parseAll(
        "module M; class Foo public method run() "
        "try f(); catch var e: Error log(e); end; end run; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
