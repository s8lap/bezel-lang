// #2007 (§5.1 assignability, §15.1 inline-array size, §14.2 inline-array
// generic args, v0.204) — the per-argument identity predicates
// `typeArgSameBase` / `typeArgIdentical` (src/sema/ExprTypecheck.cpp) had no
// InlineArray case, so `variantGenericAssignable` rejected even an identical
// `Box<[3]int> := Box<[3]int>`. Consequence: the typed-annotation `as` form
// `var w: Box<[N]int>? := o as Box<[N]int>` (the only form that flows through
// these predicates — the inferred-`as` form was already correct under #2001)
// failed §5.1 even though every type was identical. A conservative over-reject
// (LOW-MEDIUM severity), NOT memory-unsafe; surfaced by the #2001 lane.
//
// The fix mirrors #2001's `typeRefEqual` change in the type-arg family: an
// InlineArray argument shares a base iff ownedness matches AND the folded
// sizes are equal when both are folded (per §15.1 "The size N is part of the
// type") AND the elements share a base; typeArgIdentical then recurses the
// element for full identity. A Function case is added symmetrically (mirroring
// typeRefEqual) so a function-typed argument compares by structural shape.
//
// Most tests observe a bool via the process exit code (1 = true, 0 = false);
// the typed-annotation `as` cases additionally require `r.ran` (the over-reject
// was a compile-time §5.1 error, so without the fix `r.ran == false`).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Box<U> (a one-field wrapper, inline-array-friendly) plus a Main body. Reused
// across cases.
int runProg(const std::string& mainBody) {
    return run(
        "module M; "
        "class Box<U> public V: U; "
        "  public constructor(v: U) V := v; end constructor; end Box; "
        "class C public static method Main(): int " + mainBody +
        " end Main; end C;");
}

// True iff a compile-time §5.1 over-reject fired on a runAot — the bug
// condition the typed-annotation `as` cases fail without the fix.
bool rejectedOnFiveOne(const bezel::test::JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ---- THE FIX (fail-without / pass-with): the issue's exact failing repro.
// The typed-annotation `as` form `var w: Box<[3]int>? := o as Box<[3]int>`.
// FAILS WITHOUT the fix (r.ran == false with a §5.1 over-reject — typeArgSameBase
// had no InlineArray case, so variantGenericAssignable rejected the identical
// `Box<[3]int> := Box<[3]int>` self-conversion). PASSES WITH the fix.
BEZEL_TEST(codegen_2007, typed_annotation_as_inline_array_compiles_and_runs) {
    auto r = runAot(
        "module M; "
        "class Box<U> public V: U; "
        "  public constructor(v: U) V := v; end constructor; end Box; "
        "class C public static method Main(): int "
        "  var a: [3]int; a[0] := 1; a[1] := 2; a[2] := 3; "
        "  using var b := new Box<[3]int>(a); "
        "  var o: Object := b; "
        "  var w: Box<[3]int>? := o as Box<[3]int>; "   // the over-reject site
        "  return (1 if w <> null else 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedOnFiveOne(r));
    BEZEL_EXPECT_EQ(r.exitCode, 1);   // w is non-null: `as` to identical type succeeds
}

// Companion: the direct same-type assignment `var b2: Box<[3]int> := b` — also
// routed through variantGenericAssignable → typeArgIdentical, also over-rejected
// before the fix.
BEZEL_TEST(codegen_2007, direct_same_type_assignment_compiles) {
    auto r = runAot(
        "module M; "
        "class Box<U> public V: U; "
        "  public constructor(v: U) V := v; end constructor; end Box; "
        "class C public static method Main(): int "
        "  var a: [3]int; a[0] := 4; a[1] := 5; a[2] := 6; "
        "  using var b := new Box<[3]int>(a); "
        "  var b2: Box<[3]int> := b; "                  // identity → must convert
        "  return b2.V[0]; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedOnFiveOne(r));
    BEZEL_EXPECT_EQ(r.exitCode, 4);
}

// ---- #2001 / #2008 PRECISION PRESERVED: distinct sizes are STILL distinct.
// `Box<[3]int>` does NOT convert to `Box<[4]int>` (and vice versa) — the
// InlineArray case in typeArgSameBase distinguishes folded sizes (§15.1).
// A soundness guard: if the fix ever regressed to size-blind comparison, the
// wrong-size `as` would silently succeed and downstream code read past the
// real allocation.
BEZEL_TEST(codegen_2007, distinct_sizes_still_not_assignable) {
    auto r = runAot(
        "module M; "
        "class Box<U> public V: U; "
        "  public constructor(v: U) V := v; end constructor; end Box; "
        "class C public static method Main(): int "
        "  var a: [3]int; a[0] := 1; a[1] := 2; a[2] := 3; "
        "  using var b := new Box<[3]int>(a); "
        "  var o: Object := b; "
        "  var w: Box<[4]int>? := o as Box<[4]int>; "   // wrong size → null
        "  return (1 if w = null else 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);   // `as` returned null: sizes genuinely differ
}

// ---- ELEMENT DISTINCTION PRESERVED: `Box<int>` and `Box<string>` stay
// non-identical even with the new InlineArray case in place (regression guard
// for the surrounding typeArgSameBase logic).
BEZEL_TEST(codegen_2007, distinct_elements_still_not_assignable) {
    auto r = runAot(
        "module M; "
        "class Box<U> public V: U; "
        "  public constructor(v: U) V := v; end constructor; end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<int>(7); "
        "  var o: Object := b; "
        "  var w: Box<string>? := o as Box<string>; "   // wrong element → null
        "  return (1 if w = null else 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ---- NESTED inline-array arg through the typed-annotation `as` form: identity
// at depth. `o as Box<Box<[3]int>>` against a `Box<Box<[3]int>>` value. The
// typeArgIdentical recursion (newly InlineArray-aware) must distinguish this
// from `Box<Box<[4]int>>` while accepting the matching size.
BEZEL_TEST(codegen_2007, nested_inline_array_as_right_size_non_null) {
    BEZEL_EXPECT_EQ(runProg(
        "  var inner: [3]int; inner[0] := 10; inner[1] := 20; inner[2] := 30; "
        "  using var wrap := new Box<[3]int>(inner); "
        "  using var outer := new Box<Box<[3]int>>(wrap); "
        "  var o: Object := outer; "
        "  var w := o as Box<Box<[3]int>>; "
        "  return (1 if w <> null else 0);"), 1);
}

// Nested, WRONG size → null (the size distinction recurses through the
// InlineArray case in typeArgSameBase → typeArgIdentical).
BEZEL_TEST(codegen_2007, nested_inline_array_as_wrong_size_null) {
    BEZEL_EXPECT_EQ(runProg(
        "  var inner: [3]int; inner[0] := 10; inner[1] := 20; inner[2] := 30; "
        "  using var wrap := new Box<[3]int>(inner); "
        "  using var outer := new Box<Box<[3]int>>(wrap); "
        "  var o: Object := outer; "
        "  var w := o as Box<Box<[4]int>>; "
        "  return (1 if w = null else 0);"), 1);
}
