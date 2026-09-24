// fix #4871 — §22.15 (v0.307) rejects `pure` on a destructor: "disposal
// is the effect context purity fences off." Rule-corpus s22_15_0011 is
// the spec's own example.
//
// Root cause: the existing sema arm at TypeResolution_Checks.cpp:15442-15454
// is dead code in the current AST — `DestructorDecl` does not carry the
// modifiers vector (same shape as the async-rejection bug fixed in #4414),
// so sema has no `pure` flag to inspect. The fix is at parse time, in
// src/parser/Parser_Decls.cpp, mirroring the existing async rejection at
// lines 1589-1596. The dead sema arm is left in place defensively, with
// the SAME diagnostic text so the §22.15 needle is byte-identical between
// the parser-side (active) and sema-side (defensive) rejections.
//
// REJECTED — fails without the parser patch (silent parse, no diagnostic)
// and passes with the parser patch (parse emits the §22.15-named
// diagnostic).

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;

namespace {

// True iff the diagnostics carry an error mentioning §22.15 — the §22.15
// `pure is rejected on destructors` rule. Pinning on the section number
// (not just the substring `pure`) so unrelated pure-bearing diagnostics
// do not pass.
bool hasPureDestructorError(const bezel::test::ParseResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("22.15") != std::string::npos &&
            d.message.find("destructor") != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

// REJECTED: `pure` on a destructor. Pre-fix the modifier was dropped
// along with the diagnostic about destructors having no visibility — the
// parse succeeded silently and the destructor body executed as if `pure`
// had not been written. Post-fix the parser emits the §22.15-named
// diagnostic and the destructor is rejected at parse time.
BEZEL_TEST(parser_pure_destructor_4871, pure_on_destructor_rejected) {
    auto r = parseAll(
        "module M; public class C "
        "  destructor end destructor; "
        "end C;");
    // baseline sanity: an unadorned destructor parses cleanly.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());

    auto r2 = parseAll(
        "module M; public class C "
        "  pure destructor end destructor; "
        "end C;");
    BEZEL_EXPECT_TRUE(r2.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasPureDestructorError(r2));
}

// POSITIVE control: `pure` on the instance constructor is the existing
// §22.15 example and must still parse without error. The fix must NOT
// over-reject — the constructor AST does propagate `isPure` (per
// Parser_Decls.cpp:1502-1528), and the new destructor-side rejection
// is local to that branch.
BEZEL_TEST(parser_pure_destructor_4871, pure_on_constructor_accepted) {
    auto r = parseAll(
        "module M; public class C "
        "  public pure constructor() end constructor; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// POSITIVE control: `pure` on a regular method is the canonical §22.15
// "pure member" example and must still parse without error. The fix
// must NOT bleed into the method-parsing path (the method AST does
// propagate `isPure` via parseMethodDeclBody).
BEZEL_TEST(parser_pure_destructor_4871, pure_on_method_accepted) {
    auto r = parseAll(
        "module M; public class C "
        "  public pure method Foo(): int "
        "    return 0; "
        "  end Foo; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
