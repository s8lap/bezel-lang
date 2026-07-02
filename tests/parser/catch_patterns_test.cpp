// Spec-derived parser tests for the extended catch-arm patterns — §A.6:
//
//   CatchArm     ::= CatchPattern ("when" Expression)? "=>" ArmBody
//   CatchPattern ::= Type ("," Type)*                          /* no binding */
//                  | "var" Identifier ":" Type ("," Type)*      /* binding */
//
// Slice 22 covered only the single-Type binding form. Slice 43 adds the
// no-binding form, multi-Type lists in both forms, and the optional
// `when EXPR` guard. ArmBody is parsed as a single Statement (a
// `do … end` block fits that shape via DoEndBlock).

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;
using bezel::test::stmtSummary;

namespace {

const bezel::ast::TryStmt* extract(const bezel::test::ParseResult& r) {
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return nullptr;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td || td->members.empty()) return nullptr;
    const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(
        td->members[0].get());
    if (!md || md->stmts.empty()) return nullptr;
    return dynamic_cast<const bezel::ast::TryStmt*>(md->stmts[0].get());
}

std::string renderBody(const std::string& body) {
    const std::string src =
        "module M; class C public method M() " + body +
        " end M; end C;";
    auto r = parseAll(src);
    if (r.diagnostics.hasErrors()) return "<errors>";
    const auto* t = extract(r);
    return t ? stmtSummary(*t) : std::string("<no-try>");
}

} // namespace

BEZEL_TEST(parser_catch_pattern, no_binding_single_type) {
    BEZEL_EXPECT_EQ(
        renderBody("try risky(); catch IOException => log(); end;"),
        std::string(
            "try { risky(); } catch IOException => log(); end;"));
}

BEZEL_TEST(parser_catch_pattern, no_binding_multi_type) {
    BEZEL_EXPECT_EQ(
        renderBody(
            "try risky(); catch IOException | NetException => log(); end;"),
        std::string(
            "try { risky(); } "
            "catch IOException | NetException => log(); end;"));
}

BEZEL_TEST(parser_catch_pattern, binding_multi_type) {
    BEZEL_EXPECT_EQ(
        renderBody(
            "try risky(); catch var e: IOException | NetException "
            "=> log(e); end;"),
        std::string(
            "try { risky(); } "
            "catch var e: IOException | NetException => log(e); end;"));
}

BEZEL_TEST(parser_catch_pattern, binding_single_type_with_guard) {
    BEZEL_EXPECT_EQ(
        renderBody(
            "try risky(); catch var e: Error when e.IsFatal "
            "=> rethrow(e); end;"),
        std::string(
            "try { risky(); } catch var e: Error when e.IsFatal "
            "=> rethrow(e); end;"));
}

BEZEL_TEST(parser_catch_pattern, no_binding_with_guard) {
    BEZEL_EXPECT_EQ(
        renderBody(
            "try risky(); catch IOException when ShouldRetry() "
            "=> retry(); end;"),
        std::string(
            "try { risky(); } catch IOException when ShouldRetry() "
            "=> retry(); end;"));
}

BEZEL_TEST(parser_catch_pattern, do_end_arm_body) {
    // Multi-statement bodies via DoEndBlock (the §A.6 DoEndBlock fits the
    // single-Statement slot per the §A.7 ArmBody note).
    BEZEL_EXPECT_EQ(
        renderBody(
            "try risky(); catch var e: Error => "
            "do log(e); cleanup(); end; end;"),
        std::string(
            "try { risky(); } catch var e: Error => "
            "do { log(e); cleanup(); } end; end;"));
}

BEZEL_TEST(parser_catch_pattern, multiple_arms_mixed_shapes) {
    BEZEL_EXPECT_EQ(
        renderBody(
            "try risky(); "
            "catch IOException => retry();, "
            "catch var e: Error when e.IsFatal => abort(e); "
            "end;"),
        std::string(
            "try { risky(); } "
            "catch IOException => retry(); "
            "catch var e: Error when e.IsFatal => abort(e); end;"));
}

BEZEL_TEST(parser_catch_pattern, original_binding_form_still_works) {
    // Regression for slice 22's tests — the original single-Type binding
    // pattern with single-Stmt body should remain functional.
    BEZEL_EXPECT_EQ(
        renderBody("try risky(); catch var e: Error => log(e); end;"),
        std::string(
            "try { risky(); } catch var e: Error => log(e); end;"));
}
