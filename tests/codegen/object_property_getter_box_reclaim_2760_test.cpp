// Codegen / §8.6 boxing — issue #2760: the §8.6 box an `Object`-typed property
// getter mints at its return edge was never reclaimed — 24 B per read.
// Measured on `d617470e` with the issue's own repro scaled to x500:
// `SUMMARY: AddressSanitizer: 12000 byte(s) leaked in 500 allocation(s)`
// (the issue filed it at x64 = 1536 B / 64, the same 24 B per read), and
// byte-identical for the expression-bodied twin #2761 recorded in its comment
// thread — "one question with two call sites, not two independent leaks".
//
// #2760 ASKED FOR A RULING BEFORE IMPLEMENTING; §28.9 GIVES ONE. The issue set
// out two readings — (1) the box is caller-owned like the method form, (2) A4
// forbids the shape entirely — and declined to guess. At v0.306 §28.9's
// *Accessor faces are uniform across type kinds* paragraph settles both halves:
//
//   "A property or indexer read is a lend for every declared type … a getter
//    never transfers a reclamation obligation to its reader, and the two
//    accessor body spellings of §23.3 (`get => e` and `get => do … end`) are
//    semantically indistinguishable at every read site. A getter body whose
//    result is a fresh runtime-managed production … remains admissible —
//    runtime-managed values are exempt from the ceremony — and the
//    implementation reclaims the production; how is not specified."
//
// Reading (2) is REFUTED — the shape "remains admissible" — and the leak is a
// defect, because the implementation is *required* to reclaim. The same
// sentence makes the two §23.3 body spellings one question, which is why cases
// (A) and (B) below are the same program twice.
//
// ⚠ AND THE FIX DOES NOT CLOSE THAT DIVERGENCE EVERYWHERE — only at the
// LOCAL-BIND site these rows cover. MEASURED on the fixed tree, at an
// UNBOUND type-test site (`if h.O is int then …`, x200): the block-bodied
// getter still leaks 4800 B / 200 while the expression-bodied one is clean.
// The reason is structural: `isUserPropertyBoxRead` went into
// `boxLocalAssignmentSourceKind` (the binding classifier) only, while
// `lowerTypeTest`'s `freshBoxTemp` still asks `lastBoxMint_ ||
// isUserMethodBoxReturnCall` — which the inlined expression form satisfies
// by mint identity and the compiled block form cannot. Not a regression
// (both forms leaked at every site before), but the §28.9 sentence quoted
// above is satisfied here only for the bound face; the unbound face is
// tracked as issue #4497. The `how` the spec leaves open
// is the recorded design choice: the READER'S BINDING discharges it at that
// binding's release point — the mechanism `boxReturnMethods_` already gives the
// method spelling — so no obligation reaches user code and the accessor face
// stays a lend (§28.9 / §28.6 / A4). §8.6 supplies the rest: "Boxed values are
// runtime-managed (§28.9)". Spec revision v0.306.
//
// THE DEFECT. `isPlusOneBoxProducer` recognized two +1 box producers — a
// boxable-typed initializer (case (a)) and a call to a `boxReturnMethods_`
// method (case (b)) — and a property read is neither: `h.O` has static type
// `Object` at every read site, so case (a) fails, and it is a
// `MemberAccessExpr`, not a `CallExpr`, so case (b) fails. The reader's local
// never joined `boxReleaseEligible_` and no `bz_box_release` was ever emitted.
// The byte-identical METHOD spelling (`public method GetO(): Object return _n;
// end GetO;`) was already clean, which is what made this a gap rather than a
// design position.
//
// WHY NON-VIRTUAL IS LOAD-BEARING, not tidiness: the classification is read off
// the statically-resolved property's OWN getter body, and an override can
// return a class reference through the same `Object` face. Handing a class
// instance to `bz_box_release` corrupts the word at offset 8 — a UAF, strictly
// worse than the leak. Cases (C) and (D) pin the excluded shapes.
//
// WARNING Every case is LOOP-AMPLIFIED (x200). A one-shot LSan row is not an
// oracle here: a single 24-byte box is routinely still reachable from a live
// stack slot or register at exit.
//
// MECHANICAL FAIL -> PASS (`--sanitize=address`, per case):
//   (A) 4800 B / 200 allocs -> clean   (block-bodied getter, the issue's repro)
//   (B) 4800 B / 200 allocs -> clean   (expression-bodied getter, #2761's twin)
//   (C) clean before AND after         (class- and string-returning getters —
//                                       a release here would be a UAF)
//   (D) leaks before AND after         (mixed returns: not every exit boxes)
// The PLAIN (unsanitized) exit code is identical before and after in every
// case — a leak is invisible without a sanitizer.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked / …Lenient
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// =====================================================================
// (A) THE ISSUE'S OWN REPRO, loop-amplified: a BLOCK-bodied `Object`
// getter over an `int` field, read into a local. §23.3 / §8.6 / §28.9.
// =====================================================================
const char* kBlockBodiedGetter =
    "module M; "
    "class Holder "
    "  private _n: int; "
    "  public constructor(n: int) _n := n; end constructor; "
    "  public property O: Object "
    "    get => do "
    "      return _n; "
    "    end; "
    "  end O; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var hits := 0; "
    "    for var i in 1..200 do "
    "      using var h := new Holder(i); "
    "      var o := h.O; "
    "      if o is int then hits := hits + 1; end; "
    "    end; "
    "    if hits = 200 then return 7; end; "
    "    return 1; "
    "  end Main; "
    "end P;";

// =====================================================================
// (B) THE EXPRESSION-BODIED TWIN (#2761), byte-identical leak. §28.9 makes
// the two §23.3 body spellings "semantically indistinguishable at every
// read site", so one predicate must cover both — the only implementation
// difference is that this form is INLINED at the read site (the mint lands
// in the caller's frame) while (A) is compiled to `bz$prop$O$get` and
// returns the box across the frame boundary. That inlining is also exactly
// why the two forms still DIVERGE at an unbound type-test site, which this
// fix does not reach: see the ⚠ note in the file header.
// =====================================================================
const char* kExpressionBodiedGetter =
    "module M; "
    "class Holder "
    "  private _n: int; "
    "  public constructor(n: int) _n := n; end constructor; "
    "  public property O: Object => _n; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var hits := 0; "
    "    for var i in 1..200 do "
    "      using var h := new Holder(i); "
    "      var o := h.O; "
    "      if o is int then hits := hits + 1; end; "
    "    end; "
    "    if hits = 200 then return 7; end; "
    "    return 1; "
    "  end Main; "
    "end P;";

// =====================================================================
// (C) CONTROL — `Object`-typed getters that mint NO box: one returns a
// class reference, one a string. §8.6 boxes only a VALUE type at an
// `Object` location; a class reference and a `string` keep their own
// representation in the slot (§4.6: "string is a reference type … its
// descriptor exists only so `o is string` has a target"). Classifying
// either as a +1 box producer would hand a class instance or a
// `%bz.string` to `bz_box_release`, whose rc word lands at offset 8 —
// inside the instance's disposed-flag word. This row must be CLEAN under
// the strict oracle: nothing is leaked, and nothing may be freed.
// =====================================================================
const char* kNonBoxingGetters =
    "module M; "
    "class Thing "
    "  public N: int := 0; "
    "  public constructor(n: int) N := n; end constructor; "
    "end Thing; "
    "class Holder "
    "  private _t: Thing; "
    "  public constructor(t: Thing) _t := t; end constructor; "
    "  public property AsObj: Object => _t; "
    "  public property AsStr: Object => \"hi\"; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var acc := 0; "
    "    for var i in 1..200 do "
    "      using var t := new Thing(i); "
    "      using var h := new Holder(t); "
    "      var a := h.AsObj; "
    "      if a is Thing then acc := acc + 1; end; "
    "      var s := h.AsStr; "
    "      if s is string then acc := acc + 1; end; "
    "    end; "
    "    if acc = 400 then return 7; end; "
    "    return 1; "
    "  end Main; "
    "end P;";

// =====================================================================
// (D) CONTROL — a getter whose returns are MIXED: one exit yields the
// `int` field (boxed), the other a class reference (not boxed). The
// classification is all-or-nothing, exactly as `collectBoxReturnMethods`
// requires of a method ("every ReturnStmt … carries a value whose static
// type is boxable"), because the release site cannot see which exit ran.
// So this getter is NOT a +1 producer and its boxed exits keep the
// pre-existing conservative leak.
//
// Lenient checked oracle: LSan off (the leak is the point), generated-code
// AddressSanitizer ON — so if the all-or-nothing gate is ever loosened,
// the class-reference exit is released as a box and this row goes red.
// =====================================================================
const char* kMixedReturnGetter =
    "module M; "
    "class Thing "
    "  public N: int := 0; "
    "  public constructor(n: int) N := n; end constructor; "
    "end Thing; "
    "class Holder "
    "  private _n: int; "
    "  private _t: Thing; "
    "  public constructor(n: int, t: Thing) _n := n; _t := t; end constructor; "
    "  public property Mixed: Object "
    "    get => do "
    "      if _n > 100 then return _t; end if; "
    "      return _n; "
    "    end; "
    "  end Mixed; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var ints := 0; "
    "    var refs := 0; "
    "    for var i in 1..200 do "
    "      using var t := new Thing(i); "
    "      using var h := new Holder(i, t); "
    "      var m := h.Mixed; "
    "      if m is int then ints := ints + 1; end; "
    "      if m is Thing then refs := refs + 1; end; "
    "    end; "
    "    if ints = 100 then "
    "      if refs = 100 then return 7; end; "
    "    end; "
    "    return 1; "
    "  end Main; "
    "end P;";

}  // namespace

// ---------------------------------------------------------------------
// (A) the issue's own repro — block-bodied getter.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_object_property_getter_box_reclaim_2760,
           block_bodied_object_getter_box_is_reclaimed_spec_28_9) {
    JitResult r = runAotChecked(kBlockBodiedGetter);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);          // pre-fix: 4800 B / 200 allocations
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(codegen_object_property_getter_box_reclaim_2760,
           block_bodied_object_getter_plain_value_invariant_spec_23_3) {
    // NOT a #2760 oracle — a leak is invisible without a sanitizer. It pins
    // that the fix did not change the read's value, i.e. that the box is not
    // freed before `o is int` reads its descriptor.
    JitResult r = runAot(kBlockBodiedGetter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ---------------------------------------------------------------------
// (B) the expression-bodied twin — §28.9 makes it the same question.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_object_property_getter_box_reclaim_2760,
           expression_bodied_object_getter_box_is_reclaimed_spec_28_9) {
    JitResult r = runAotChecked(kExpressionBodiedGetter);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);          // pre-fix: 4800 B / 200 allocations
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(codegen_object_property_getter_box_reclaim_2760,
           expression_bodied_object_getter_plain_value_invariant_spec_23_3) {
    JitResult r = runAot(kExpressionBodiedGetter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ---------------------------------------------------------------------
// (C) CONTROL: getters that mint no box must not be classified.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_object_property_getter_box_reclaim_2760,
           class_and_string_returning_getters_are_not_box_producers_spec_8_6) {
    JitResult r = runAotChecked(kNonBoxingGetters);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);          // clean before AND after
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(codegen_object_property_getter_box_reclaim_2760,
           class_and_string_returning_getters_plain_value_invariant_spec_8_6) {
    JitResult r = runAot(kNonBoxingGetters);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ---------------------------------------------------------------------
// (D) CONTROL: a mixed-return getter is not a +1 producer. Lenient
// checked oracle — LSan off, generated-code ASan on.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_object_property_getter_box_reclaim_2760,
           mixed_return_getter_is_not_a_box_producer_spec_28_9) {
    JitResult r = runAotCheckedLenient(kMixedReturnGetter);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);          // a release on the class exit faults
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(codegen_object_property_getter_box_reclaim_2760,
           mixed_return_getter_plain_value_invariant_spec_23_3) {
    JitResult r = runAot(kMixedReturnGetter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
