// Spec-derived tests for issue #1775 — a `for var x in <source>` whose source
// is NOT iterable must be rejected in SEMA with a §21.4 diagnostic.
//
// Root cause: sema had NO §21.4 iterability gate on the for-in source. A
// non-iterable source (a bare `int`/`bool`, an `enum` value, or a plain class
// that does not implement `IEnumerable<T>`) type-checked cleanly and then fell
// through every codegen for-shape handler to a bare "not yet implemented:
// codegen for this for statement" NYI (LlvmBackend_Stmts.cpp). §21.4 states a
// `for` source must be iterable: a value whose type implements `IEnumerable<T>`
// (§29), an inline array (§15.1.2), a string (§4.6, iterated as chars), or a
// `Range<T>`. The fix adds the check to ExprTC::visitStmt (ExprTypecheck.cpp):
// resolve the source type and, when it is a non-`string` primitive or a
// locally-declared user type with neither `GetEnumerator` nor an
// IEnumerable-family base, emit the §21.4 error before the loop-variable is
// symbolized. Valid iterables (Range / inline array / string / user
// IEnumerable<T>) are unaffected.
//
// per §21.4 (the `for` statement), §29 (iteration), issue #1775.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool anyDiagContains(const bezel::test::SemaResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(std::string(needle)) != std::string::npos) return true;
    return false;
}

// A method with a bare-`int` `n` and a `bool` `b` in scope.
std::string wrap(const std::string& body) {
    return std::string(
        "module M; class C "
        "public method M(n: int, b: bool): int ") +
        body + " end M; end C;";
}

// A class implementing IEnumerable<int> the §29-A way (GetEnumerator +
// IEnumerator<int>), reused by the accept cases.
constexpr const char* kIntRange =
    "class IntRangeEnum: IEnumerator<int> "
    "  private lo: int; private hi: int; private cur: int; "
    "  public constructor(lo: int, hi: int) this.lo:=lo; this.hi:=hi; cur:=lo-1; end constructor; "
    "  public method MoveNext(): bool cur:=cur+1; return cur<=hi; end MoveNext; "
    "  public readonly property Current: int get => cur; end Current; "
    "end IntRangeEnum; "
    "class IntRange: IEnumerable<int> "
    "  private lo: int; private hi: int; "
    "  public constructor(lo: int, hi: int) this.lo:=lo; this.hi:=hi; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> return new IntRangeEnum(lo, hi); end GetEnumerator; "
    "end IntRange; ";

}  // namespace

// ── Rejects: non-iterable sources now diagnosed at §21.4 ──────────────────────

// §21.4: a bare `int` is not iterable. (This is the exact shape of the #1775
// repro and the former break_continue `continue_in_for_ok` program.)
BEZEL_TEST(sema_for_iterability_1775, int_source_rejected_spec_21_4) {
    auto r = parseSemaAll(wrap("for var x in n do continue; end; return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "21.4"));
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "iterable source"));
}

// §21.4: a `bool` is not iterable.
BEZEL_TEST(sema_for_iterability_1775, bool_source_rejected_spec_21_4) {
    auto r = parseSemaAll(wrap("for var x in b do continue; end; return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "21.4"));
}

// §21.4: an explicit loop-variable annotation does NOT rescue a non-iterable
// source — the source type, not the loop variable, must be iterable.
BEZEL_TEST(sema_for_iterability_1775, annotated_int_source_rejected_spec_21_4) {
    auto r = parseSemaAll(wrap("for var x: int in n do continue; end; return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "21.4"));
}

// §21.4 / §29: a plain class that does not implement IEnumerable<T> is not
// iterable.
BEZEL_TEST(sema_for_iterability_1775, non_ienumerable_class_rejected_spec_21_4) {
    auto r = parseSemaAll(
        "module M; class Widget public value: int := 0; end Widget; "
        "class C public method M(w: Widget): int "
        "  for var x in w do continue; end; return 0; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "21.4"));
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "iterable source"));
}

// §21.4 / §10: an enum value is not iterable.
BEZEL_TEST(sema_for_iterability_1775, enum_source_rejected_spec_21_4) {
    auto r = parseSemaAll(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public method M(c: Color): int "
        "  for var x in c do continue; end; return 0; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "21.4"));
}

// ── Accepts: valid iterables still type-check cleanly (no §21.4) ──────────────

// §21.4: a `Range<int>` (`0..n`) is iterable.
BEZEL_TEST(sema_for_iterability_1775, range_source_accepted) {
    auto r = parseSemaAll(wrap("for var x in 0..n do continue; end; return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(anyDiagContains(r, "21.4"));
}

// §15.1.2: an inline array `[N]T` is iterable.
BEZEL_TEST(sema_for_iterability_1775, inline_array_source_accepted) {
    auto r = parseSemaAll(wrap(
        "var a: [3]int; for var x in a do continue; end; return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(anyDiagContains(r, "21.4"));
}

// §4.6: a string is iterable (yields chars).
BEZEL_TEST(sema_for_iterability_1775, string_source_accepted) {
    auto r = parseSemaAll(wrap(
        "var s := \"abc\"; for var c in s do continue; end; return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(anyDiagContains(r, "21.4"));
}

// §29-A: a user type implementing IEnumerable<T> (GetEnumerator) is iterable.
BEZEL_TEST(sema_for_iterability_1775, user_ienumerable_source_accepted) {
    auto r = parseSemaAll(
        std::string("module M; ") + kIntRange +
        "class C public method M(r: IntRange): int "
        "  var s := 0; for var x in r do s := s + x; end; return s; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(anyDiagContains(r, "21.4"));
}

// §29: a variable typed as the builtin `IEnumerable<int>` interface is iterable.
BEZEL_TEST(sema_for_iterability_1775, ienumerable_interface_param_accepted) {
    auto r = parseSemaAll(
        "module M; class C public method M(xs: IEnumerable<int>): int "
        "  var s := 0; for var x in xs do s := s + x; end; return s; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(anyDiagContains(r, "21.4"));
}
