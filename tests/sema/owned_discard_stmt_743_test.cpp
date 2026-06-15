// Spec-derived sema tests for fix #743 — discarding a freshly OWNED value that
// requires disposal as a bare expression statement is a §21.9 hard error.
//
// Authority: §21.9 — "For non-trivial-dispose class types and for Task, the
// warning is replaced by a hard error directing the caller to
// `using var _ := expression;`, an explicit `.Dispose()`, or
// `Task.FireAndForget` as appropriate."
//
// The check is gated on the value being owned-PRODUCED at the statement (a
// `new T()` or a call whose method returns `owned T`), so the ubiquitous
// borrowed-return statement `obj.Foo();` is never false-rejected.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasDiscardLeakError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("21.9") != std::string::npos &&
            d.message.find("leaks the resource") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// §21.9: `new T();` where T is non-trivial-dispose (a destructor) — hard error.
BEZEL_TEST(owned_discard_743, new_non_trivial_dispose_is_error) {
    auto r = parseSemaAll(
        "module M; "
        "class R public _h: int := 0; destructor _h := 1; end destructor; end R; "
        "class C public static method Main(): int "
        "  new R(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasDiscardLeakError(r));
}

// §21.9: a call returning `owned T` (T non-trivial-dispose) discarded — error.
BEZEL_TEST(owned_discard_743, owned_returning_call_is_error) {
    auto r = parseSemaAll(
        "module M; "
        "class R public _h: int := 0; destructor _h := 1; end destructor; end R; "
        "class F public static method Make(): owned R return new R(); end Make; end F; "
        "class C public static method Main(): int "
        "  F.Make(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasDiscardLeakError(r));
}

// §21.9: a BORROWED-return statement is NOT rejected — the caller owns nothing.
BEZEL_TEST(owned_discard_743, borrowed_return_statement_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class R public _h: int := 0; destructor _h := 1; end destructor; "
        "  public method Touch(): int return _h; end Touch; end R; "
        "class C public static method Main(): int "
        "  using var r := new R(); "
        "  r.Touch(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasDiscardLeakError(r));
}

// §21.9: `new T();` for a TRIVIAL-dispose class (no destructor) — accepted.
BEZEL_TEST(owned_discard_743, new_trivial_dispose_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class Plain public _n: int := 0; end Plain; "
        "class C public static method Main(): int "
        "  new Plain(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasDiscardLeakError(r));
}

// §21.9: a void-returning method call statement is unaffected.
BEZEL_TEST(owned_discard_743, void_call_statement_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Noop() end Noop; "
        "  public static method Main(): int "
        "    Noop(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasDiscardLeakError(r));
}
