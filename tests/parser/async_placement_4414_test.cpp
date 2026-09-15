// fix #4414 — §30.1 (v0.300) restricts `async` to an async method, an async
// lambda, or an iterator method. The shared member-modifier prefix over-
// admitted `async` at the parser, so `public async constructor()` and
// `public async destructor()` parsed silently with the modifier dropped.
// Per §30.1 itself: "This rule is enforced by the grammar, not semantically:
// Appendix A.4/A.7 admit `async` in the `MethodDecl` and `LambdaExpr`
// productions only, so `async` on a constructor, destructor, or accessor
// is a parse error." The ConstructorDecl / DestructorDecl ASTs do not
// preserve the modifiers vector, so the rejection happens at parse time
// (same shape as the existing `pure`-not-admitted-on-event check).
//
// REJECTED — each must FAIL without the fix (silent parse, no diagnostic),
// and PASS with the fix (parse emits a §30.1-named diagnostic).

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;

namespace {

// True iff the diagnostics carry an error mentioning §30.1 — the §30.1
// `async` placement rule. Pinning on the section number (not just the
// substring `async`) so unrelated async-bearing diagnostics do not pass.
bool hasAsyncPlacementError(const bezel::test::ParseResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("30.1") != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// REJECTED: `async` on an instance constructor. Pre-fix this parsed
// silently and dropped the modifier (the constructor body executed as a
// normal synchronous body).
BEZEL_TEST(parser_async_placement_4414, async_on_constructor_rejected) {
    auto r = parseAll(
        "module M; public class C "
        "  public async constructor() end constructor; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAsyncPlacementError(r));
}

// REJECTED: `async` on a static-constructor form. Same rule — only methods
// and lambdas carry `async`.
BEZEL_TEST(parser_async_placement_4414, async_on_static_constructor_rejected) {
    auto r = parseAll(
        "module M; public class C "
        "  static async constructor() end constructor; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAsyncPlacementError(r));
}

// REJECTED: `async` on a destructor. Pre-fix this parsed silently too —
// the modifier was dropped along with the diagnostic about destructors
// having no visibility.
BEZEL_TEST(parser_async_placement_4414, async_on_destructor_rejected) {
    auto r = parseAll(
        "module M; public class C "
        "  async destructor() end destructor; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAsyncPlacementError(r));
}

// REJECTED: `async` on a property accessor. The property-accessor
// production (`Visibility? "pure"? ("get" | "set")`) does not admit
// `async`, so `async get` already fails to parse pre-fix (the parser
// never reaches parsePropertyAccessor — isPropertyAccessorStart returns
// false for `async`). What changed in this fix is that the BUG case
// (silent parse + modifier drop) is now closed for the two positions
// where it actually happened: constructors and destructors. This test
// pins the accessor path as REJECTED (cascading parse error) without
// claiming a §30.1-named diagnostic — the actual §30.1-named
// diagnostic-improvement for accessors is a separate scope.
BEZEL_TEST(parser_async_placement_4414, async_on_accessor_rejected) {
    auto r = parseAll(
        "module M; public class C "
        "  public property P: int "
        "    async get begin return 0; end get; "
        "  end property; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// POSITIVE control: `public async method Foo(): int … end Foo;` is the
// canonical §30.1 example and must still parse without error. Locks in
// that the fix does not over-reject.
BEZEL_TEST(parser_async_placement_4414, async_method_accepted) {
    auto r = parseAll(
        "module M; public class C "
        "  public async method Foo(): Task<int> "
        "    return await Task.FromResult(0); "
        "  end Foo; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// POSITIVE control: a method whose modifier order is `static async method …`
// (§30.1 "Modifier order: `visibility static async ... method ...`").
BEZEL_TEST(parser_async_placement_4414, public_static_async_method_accepted) {
    auto r = parseAll(
        "module M; public class C "
        "  public static async method Bar(): Task "
        "    return; "
        "  end Bar; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}