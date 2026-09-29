// Spec-derived regression tests for fix #4583 — the codegen half of the
// §27.8 (v0.306) alias-transparency defect split from #4570.
//
// THE BUG. #4570 fixed sema's `namedTypeSimpleName` so a type alias of an
// arity-1 builtin generic type-checks transparently. It explicitly did NOT
// fix the codegen twin: `isSpanLikeTypeName` and `isOwningSpanTypeName`
// (src/codegen/LlvmBackend_impl.hpp) read `rt->astNode->name->parts[0]`
// directly instead of going through `TypeRef::headName()`. §27.8 alias
// resolution (`Resolver::resolveNamed`, TypeResolution_Names.cpp) copies the
// underlying TypeRef and then RESTORES `astNode` to the USE SITE (the alias
// name), so for `type Al := ArraySpan<int>; var s: Al := ...;` the raw read
// answered `"Al"`, matched neither predicate, and every MEMBER ACCESS
// through `s` fell through the span-struct lowering entirely and failed to
// lower — an internal error ("return-value expression failed to lower to a
// value ... #2491"), not a rejection. The alias-typed LOCAL itself lowered
// fine (its slot is untyped bytes); only reads *through* it broke.
//
// ROOT CAUSE — CONFIRMED, NOT ASSUMED (per #4583's own instruction to
// verify the #4570 shape transfers before copying it). `ctx_.exprType(*ma-
// >target)` for an alias-typed span local carries a POPULATED
// `aliasResolvedHead` (e.g. "ArraySpan") at the exact site
// (LlvmBackend_Exprs.cpp's `.Length`/`.IsEmpty`/`.Start` reader) that
// dispatches on `isSpanLikeTypeName`. `Span`/`ArraySpan`/`ReadOnlySpan`/
// `ReadOnlyArraySpan` are pure syntactic builtins with NO TypeSymbol
// (`declaration` is always null for them — confirmed: no TypeResolution
// code path attaches a declaration to any of the four names), so alias
// resolution's `ref = *underlying` copies `declaration = nullptr` through
// unchanged and the existing `rt->declaration` guard in both predicates is
// unaffected. The shape is a byte-for-byte match of #4570's model: swap the
// raw `astNode->name->parts[0]` read for `rt->headName()`.
//
// NOT FIXED HERE, ON PURPOSE: `isIReadOnlySpanType`. It has the same
// `astNode->name->parts[0]` shape textually, but `IReadOnlySpan` IS backed
// by a real TypeSymbol, so its FIRST branch (`rt->declaration->name ==
// "IReadOnlySpan"`) already answers correctly through an alias — `ref =
// *underlying` copies `declaration` too. Measured (see the comment on
// `isIReadOnlySpanType`, LlvmBackend_impl.hpp): both a `for`-in loop and the
// boxed-`SequenceEqual`-argument path, each over an alias-typed
// `IReadOnlySpan<int>`, are correct on the PRE-fix compiler. No surface
// alias program was found that reaches the astNode-fallback leg, so no
// change was made there — a change with no FAIL-WITHOUT repro is not a
// provable fix (CLAUDE.md's oracle discipline). The
// `ireadonlyspan_alias_for_in_anti_regression_4583` row below pins that
// this correct (both before and after) behaviour is not disturbed by the
// isSpanLikeTypeName/isOwningSpanTypeName change.
//
// Spec: §27.8 (v0.306) alias interchangeability; §4.14 (owning
// Span<T>/ReadOnlySpan<T> over OwningBuffer<T>); §4.15 (borrowing
// ArraySpan<T>/ReadOnlyArraySpan<T> over Array<T>); §4.15/#1245
// (read-only span is not assignable to its writable counterpart).

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {
// -999 on a compile-time rejection or a failed-to-run program, so a pre-fix
// degraded-to-zero return can never collide with a post-fix value and pass
// vacuously (the pre-fix ICE is a NON-FATAL backstop diagnostic — bezelc
// still emits a binary that runs and returns 0/null, per #2491 — so the
// distinguishing signal is the WRONG VALUE, not a compile failure).
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// §4.14 class-typed element: an `OwningBuffer<R>` slot OWNS its referent, so
// the owning-Span surface that `isOwningSpanTypeName` gates (the disposing
// indexer-set and `move sp[i]`) has something observable to get wrong. R
// counts live instances, so a missed disposal shows up as a non-zero R.n.
const char* kR =
    "class R public static n: int := 0; public V: int; "
    "  public constructor(v: int) R.n := R.n + 1; V := v; end constructor; "
    "  destructor R.n := R.n - 1; end destructor; end R; ";
}  // namespace

// ============================================================================
// §4.15 — ArraySpan<int> through an alias. The issue's own repro shape:
// `.Length` in RETURN position. Two elements, so the correct answer (2) is
// distinct from the pre-fix degraded answer (0).
// ============================================================================

BEZEL_TEST(span_alias_member_access_4583, arrayspan_alias_length_return_4_15) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type ArAl4583 := ArraySpan<int>; "
        "public class C "
        "  public static method Main(): int "
        "    var a := new Array<int>(); "
        "    a.Add(5); a.Add(6); "
        "    var s: ArAl4583 := a.AsSpan(); "
        "    return s.Length; "
        "  end Main; "
        "end C;"), 2);
}

// Same shape, but the measured table's SECOND failing row: the member read
// lands in a LOCAL DECL INITIALIZER, not a return expression — a different
// lowering call site (`lowerLocalDecl`'s initializer) than the row above.
BEZEL_TEST(span_alias_member_access_4583,
           arrayspan_alias_length_local_decl_init_4_15) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type ArAl4583b := ArraySpan<int>; "
        "public class C "
        "  public static method Main(): int "
        "    var a := new Array<int>(); "
        "    a.Add(5); a.Add(6); "
        "    var s: ArAl4583b := a.AsSpan(); "
        "    var n: int := s.Length; "
        "    return n; "
        "  end Main; "
        "end C;"), 2);
}

// The issue's third failing row: the INDEXER, not `.Length`. Three distinct
// values so a wrong-index regression would not read as a pass.
BEZEL_TEST(span_alias_member_access_4583, arrayspan_alias_indexer_4_15) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type ArAl4583c := ArraySpan<int>; "
        "public class C "
        "  public static method Main(): int "
        "    var a := new Array<int>(); "
        "    a.Add(50); a.Add(60); a.Add(70); "
        "    var s: ArAl4583c := a.AsSpan(); "
        "    return s[1]; "
        "  end Main; "
        "end C;"), 60);
}

// §4.15 — ReadOnlyArraySpan<int> through an alias (the issue's repro
// reproduces "identically" on this borrowing read-only view, not just the
// writable one). Length + indexer combined into one asserted value so
// dropping either read would fail.
BEZEL_TEST(span_alias_member_access_4583,
           readonlyarrayspan_alias_length_and_indexer_4_15) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type RoAl4583 := ReadOnlyArraySpan<int>; "
        "public class C "
        "  public static method Main(): int "
        "    var a := new Array<int>(); "
        "    a.Add(7); a.Add(8); a.Add(9); "
        "    var s: RoAl4583 := a.AsReadOnlySpan(); "
        "    return s.Length + s[1]; "
        "  end Main; "
        "end C;"), 11);  // Length=3, s[1]=8 -> 11
}

// ============================================================================
// §4.14 — the OWNING Span<int> over an OwningBuffer<int>, through an alias.
// The issue's measured table: "same three rows with `type Al := Span<int>`
// over an `OwningBuffer<int>` — ICE, identically". Exercises
// isOwningSpanTypeName's alias-blind read specifically (isSpanLikeTypeName
// alone would already recognise `Span` as span-LIKE for Length/indexer, but
// isOwningSpanTypeName gates the owning-only surface — this row's `Length +
// indexer` alone does not distinguish the two predicates; the constructor
// call `new Span<int>(buf, ...)` below is unaffected by either, it types via
// sema — see the note above the next test for a row that DOES pin
// isOwningSpanTypeName specifically).
// ============================================================================

BEZEL_TEST(span_alias_member_access_4583,
           owning_span_alias_length_and_indexer_4_14) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type SpAl4583 := Span<int>; "
        "public class C "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(4); "
        "    buf[0] := 10; buf[1] := 20; buf[2] := 30; buf[3] := 40; "
        "    var s: SpAl4583 := new Span<int>(buf, 0, 4); "
        "    return s.Length + s[2]; "
        "  end Main; "
        "end C;"), 34);  // Length=4, s[2]=30 -> 34
}

// ============================================================================
// §4.14 — `isOwningSpanTypeName` through an alias. That predicate is consulted
// at exactly two codegen sites, both in LlvmBackend_Assign.cpp: the disposing
// indexer-SET (which selects the element disposer thunk) and `move sp[i]`.
// Alias-blind, both answered false for an alias-typed local, so the OWNING
// behaviour silently degraded to the borrowing one — no ICE, just a leak and a
// null.
//
// NOT used here: `Clear()`. An earlier draft of this file pinned the predicate
// with `Span<int>.Clear()`, which is doubly wrong. (a) `Clear` is admitted only
// on an owning `Span<T>` whose element type is a CLASS (stdlib §4.14); on
// `Span<int>` it is REJECTED, and identically so on the non-alias spelling, so
// the row failed for a reason unrelated to alias transparency. (b) Repaired to
// `Span<R>` it is VACUOUS: measured on both the pre- and post-fix compilers, an
// alias-typed `Span<R>.Clear()` leaves R.n at 0 either way, because the
// lowering `Clear` reaches never consults `isOwningSpanTypeName`. Both rows
// below were instead confirmed to FAIL on the pre-fix compiler (see each).
// ============================================================================

// Pre-fix: the displaced R(1) is never disposed — the alias-typed receiver
// fails `isOwningSpanTypeName`, so the element disposer thunk is null and the
// set overwrites an owned slot without disposing it. Measured pre-fix: 1 (one
// leaked instance). Post-fix: 0.
BEZEL_TEST(span_alias_member_access_4583,
           owning_span_alias_indexer_set_disposes_displaced_4_14) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kR +
        "type SpAl4583b := Span<R>; "
        "public class C "
        "  public static method Main(): int "
        "    do "
        "      using var buf := new OwningBuffer<R>(2); "
        "      buf[0] := new R(1); buf[1] := new R(2); "
        "      var s: SpAl4583b := buf.AsSpan(); "
        "      s[0] := new R(3); "
        "    end; "
        "    return R.n; "
        "  end Main; "
        "end C;"), 0);
}

// Pre-fix: `move s[0]` finds no owning-span branch and falls through to a plain
// READ of the slot, which yields null — so `t!.V` raises
// NullReferenceException (the harness reports exit 70) instead of returning 10.
BEZEL_TEST(span_alias_member_access_4583,
           owning_span_alias_move_slot_value_4_14) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kR +
        "type SpAl4583c := Span<R>; "
        "public class C "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<R>(2); "
        "    buf[0] := new R(10); buf[1] := new R(11); "
        "    var s: SpAl4583c := buf.AsSpan(); "
        "    using var t := move s[0]; "
        "    return t!.V; "
        "  end Main; "
        "end C;"), 10);
}

// ============================================================================
// §27.8 "Aliases of aliases are permitted" — a chained alias must be equally
// alias-transparent in codegen, exercising the propagation leg
// (`ref.aliasResolvedHead` carried forward from the inner alias) rather than
// the first-hop resolution.
// ============================================================================

BEZEL_TEST(span_alias_member_access_4583, chained_arrayspan_alias_27_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type ArInner4583 := ArraySpan<int>; "
        "type ArOuter4583 := ArInner4583; "
        "public class C "
        "  public static method Main(): int "
        "    var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); a.Add(3); "
        "    var s: ArOuter4583 := a.AsSpan(); "
        "    return s.Length; "
        "  end Main; "
        "end C;"), 3);
}

// ============================================================================
// Anti-regression: the DIRECT (non-alias) spelling was already correct and
// must stay that way — pins that `headName()` is a no-op for a non-alias
// TypeRef (`aliasResolvedHead` empty, falls through to the same
// `astNode->name->parts[0]` read the fix replaced).
// ============================================================================

BEZEL_TEST(span_alias_member_access_4583,
           direct_arrayspan_length_anti_regression_4_15) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    var a := new Array<int>(); "
        "    a.Add(5); a.Add(6); "
        "    var s: ArraySpan<int> := a.AsSpan(); "
        "    return s.Length; "
        "  end Main; "
        "end C;"), 2);
}

// isIReadOnlySpanType was investigated (see the file header and the comment
// on the function itself) and found to be already alias-transparent through
// an ordinary alias program, via its declaration-based leg — not touched by
// this fix. Pin that this stays true: a `for`-in loop over an alias-typed
// `IReadOnlySpan<int>` (dispatches through isIReadOnlySpanType in
// LlvmBackend_Stmts.cpp) must keep summing correctly.
BEZEL_TEST(span_alias_member_access_4583,
           ireadonlyspan_alias_for_in_anti_regression_4583) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type IroAl4583 := IReadOnlySpan<int>; "
        "public class C "
        "  public static method Main(): int "
        "    var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); a.Add(3); "
        "    var sp: IroAl4583 := a.AsReadOnlySpan(); "
        "    var n: int := 0; "
        "    for var v in sp do n := n + v; end for; "
        "    return n; "
        "  end Main; "
        "end C;"), 6);
}

// ============================================================================
// §4.15/#1245 soundness (pinned by #4570 at the sema layer; re-pinned here
// because it shares the exact receiver family this fix touches): an alias
// must not let a `ReadOnlyArraySpan<int>` escape into a writable
// `ArraySpan<int>` — that would allow writes through a read-only borrow.
// This is a SEMA rejection (checkAssignable, before codegen is ever
// reached), so it is unaffected by this codegen-only fix; confirmed it
// stays rejected with the fix applied, closing the loop #4570 opened.
// ============================================================================

BEZEL_TEST(span_alias_member_access_4583,
           readonly_span_alias_still_rejected_widening_4_15) {
    auto r = runAot(
        "module M; "
        "type RoAl4583b := ReadOnlyArraySpan<int>; "
        "public class C "
        "  public static method Main(): int "
        "    var a := new Array<int>(); "
        "    a.Add(1); "
        "    var ro: RoAl4583b := a.AsReadOnlySpan(); "
        "    var w: ArraySpan<int> := ro; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not implicitly convertible to the target type"));
}
