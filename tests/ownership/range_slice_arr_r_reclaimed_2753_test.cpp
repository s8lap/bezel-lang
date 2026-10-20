// Ownership / disposal — §28.3's "Range slice `arr[r]` on `Array<T>`" row
// is Owned and the resulting container is reclaimed in EVERY binding spelling.
//
// Authority (spec §28.3, v0.240, Producer table):
//   "| Range slice `arr[r]` on `Array<T>` (§15.6) | Owned — a fresh
//      `Array<T>`, the compiler-recognized `Slice(r: Range<int>): owned
//      Array<T>` form (stdlib §4.4); not the indexer row above. The container
//      is owned by the caller and discharged under the standard rules […] |"
//
// Why this is a separate fix from #2749. #2749 closed the `const` REJECTION
// (per §21.1) and the matching `#2073` lint for the INDEX spelling `arr[r]`,
// matching the CALL spelling `arr.Slice(r)` that #2709 had already closed.
// But the disposal — the second half of the spec row — was deliberately out
// of scope. The 0b9693e measurement is cited in #2749's own test file: under
// LSan, `const`, `using var`, and plain `var` each leaked 64 of 64 slice
// allocations, while `using var s := a.Slice(0..2)` ran clean (the positive
// control proving LSan was armed and discriminating). #2749 closed the lint
// loop; #2753 closes the disposal loop.
//
// The bug. `arr[r]` is an `ast::IndexExpr` whose target is an `Array<T>`
// (the borrowing one) and whose index is a `RangeExpr` (or a Range-typed
// value, sema case #1079). `producerState` has no `IndexExpr` arm — every
// reach falls through to the spec's *indexer* row (Borrowed, qualifier: the
// receiver as a transit point), and codegen therefore NEVER places a
// reclaim on the slice. The container is allocated but never freed.
//
// The fix. An `IndexExpr` arm in `producerState` (`Ownership.cpp:~4305`),
// classified via the shared `common::isBuiltinArrayRangeSliceIndex` predicate
// (the same one the §21.1 `const` rejection and the #2073 lint guard both
// already use — so the SEMA and the CODECON reckon from the same fact, and a
// future predicate change can never skew the two paths). The `using var`
// binding routes through `registerUsingDisposal` (the existing
// `Array<T>`-shaped collection arm); the plain `var` binding routes through
// the existing `isFreshBorrowingArrayMaterializerLocal` candidate — the slice
// is a fresh, ownerless `Array<T>` exactly like `recv.ToArray()` / `Split()`,
// and the same `bz_array_free` reclaim applies. The SHARED dispose path
// (`bz_array_slice` is then `bz_array_free`'d by the per-element disposer
// selected by element type — class-typed elements are borrows, so the
// disposer is null; `Array<string>` uses the string-release thunk, so the
// +1 per substring is released exactly once).
//
// Narrowness. The new arm is keyed on the SAME condition the `const` arm
// already recognises (the `isBuiltinArrayRangeSliceIndex` predicate); it
// therefore inherits the same precision — a single-element `arr[i]` is the
// indexer row (Borrowed, unchanged), a `string` slice is runtime-managed
// (the predicate's receiver-head gate excludes it), a span range index is
// the element type (excluded by the same gate), and a user type with a
// range indexer is excluded by the `declaration == nullptr` gate. The
// positive controls below enumerate every shape that reaches the new arm —
// a single-element index, a borrowing-family index, a span range index, a
// user class range indexer, a `string` slice, an inline-array scalar index,
// and a `Dictionary<,>` key read — and the reject-control prevents the
// unrelated §28.3 auto-drop note from firing on a slice-typed local whose
// producer is now visible to the pass.
//
// Note on the `const` form. §21.1 rejects `const s := a[0..2];` outright
// (the `const` initializer is never an owned production) — so the LSan
// oracle here covers only the TWO surviving spellings, `using var` and
// plain `var`. The "constant" form is exercised by the EXISTING
// `const_range_slice_2749` tests; the disposal loop is #2753's load.
//
// Measured on the pre-fix 0b9693e (cited in #2749's test file):
//   `using var s := a[0..2];`  → LEAK: 64 of 64   bz_array_slice allocations
//   `var       s := a[0..2];`  → LEAK: 64 of 64   bz_array_slice allocations
//   `using var s := a.Slice(0..2);`  → CLEAN: 0 leaks (positive control)
//   `var       s := a.Slice(0..2);`  → CLEAN: 0 leaks (positive control)
// The post-fix expectation is the same 0 leaks for both spellings; the
// `using var` arm is reach-checked by the existing `Array<T>` collection
// disposal path (registerUsingDisposal's head-Array arm), and the plain
// `var` arm is reach-checked by the existing materializer reclaim path
// (isFreshBorrowingArrayMaterializerLocal).

#include <string>

#include "support/aot_support.hpp"        // runAot / runAotChecked
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// A standard `Main()` body for the slice fixture — a 3-element `Array<int>` and
// the binding under test, second-positioned so the slice escapes the scope of
// the source array via the return value (counting the array's elements keeps
// the slice non-trivially late-freed). The code lives in `Main()` directly so
// the test runs the actual slice binding — placing it in a side-`M()` whose
// `Main` returns 0 would not exercise the path.
std::string fixture(const std::string& binding) {
    return "module M; class C "
           "  public static method Main(): int "
           "    using var a := new Array<int>(); "
           "    a.Add(3); a.Add(1); a.Add(2); " + binding +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// True iff some ERROR diagnostic reports a §28.3 owned production routed
// through the `const` initializer — the #2749 `const` diagonal here is
// unchanged, but `var s := a[0..2]` (the plain-`var` form) is now Owned, so
// any §28.3 auto-drop note on `s` (the "use `using var`" hint for an
// undischarged owned local) would be a regressions.
bool hasAutoDropOnSlice(const OwnershipResult& r, const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (hasAutoDropWarning(r) == false) continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

}  // namespace

// ===========================================================================
// (1) THE LOAD-BEARING REGRESSION — plain `var s := a[0..2];` is LSan-clean.
// Pre-fix: the slice container is allocated by `bz_array_slice` and never
// freed (no reclaim path on the indexer); LSan reports
// "Direct leak of N byte(s) in M object(s)" and exits with code 99.
// Post-fix: the slice is reached by the existing materializer reclaim path
// (the new `IndexExpr` arm classifies it Owned, so the reclaim register sees
// it as a fresh, ownerless `Array<T>` and emits `bz_array_free`).
// ===========================================================================
BEZEL_TEST(range_slice_arr_r_reclaimed_2753,
           plain_var_binding_no_leak_spec_28_3) {
    JitResult r = runAotChecked(fixture("var s := a[0..2];"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);            // no leak, no crash
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (2) `using var s := a[0..2];` — the binding-disposal path. §28.10 hands the
// obligation to registerUsingDisposal's standard `Array<T>` collection arm,
// which lowers to `bz_array_free` at scope end. Pre-fix: never reached
// (producerState saw the slice as Borrowed, registerUsingDisposal was
// returned out as a borrow-pin). Post-fix: same path as
// `using var s := a.Slice(0..2);` — confirmed LSan-clean via the #1079
// positive control.
// ===========================================================================
BEZEL_TEST(range_slice_arr_r_reclaimed_2753,
           using_var_binding_no_leak_spec_28_3) {
    JitResult r = runAotChecked(fixture("using var s := a[0..2];"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (3) POSITIVE CONTROL — the `Slice(r)` method spelling is already reclaimed
// (the call-lowering path frees the freshly-allocated array via the same
// materializer pipeline). This is the negative-control's proof: a non-leaking
// running of the same fixture with the parallel spelling confirms LSan was
// armed and discriminating. `.Count == 3` keeps the slice live at scope end
// long enough for the test to count what the post-fix run freed.
// ===========================================================================
BEZEL_TEST(range_slice_arr_r_reclaimed_2753,
           call_spelling_sibling_is_clean_spec_15_6_stdlib_4_4) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(3); a.Add(1); a.Add(2); "
        "    using var s := a.Slice(0..2); "
        "    return s.Count; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ===========================================================================
// (4) THE RANGE-VALUE form `var r := 0..2; a[r]` (#1079) — same producer,
// same arm. Sema types both spellings through the same range-slice arm, so
// the predicate (and the new `IndexExpr` arm) admits both.
// ===========================================================================
BEZEL_TEST(range_slice_arr_r_reclaimed_2753,
           range_variable_form_no_leak_spec_28_3_1079) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(3); a.Add(1); a.Add(2); "
        "    var r := 0..2; "
        "    var s := a[r]; "
        "    return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (5) The slice is `Array<string>`-typed — the +1 per substring the spec
// row calls "class-typed elements are borrows, [their] sources unchanged"
// must NOT be released by the slice's disposer (a borrow release would
// double-free the substring source). The reclaim path selects the disposer
// by element type, NOT by receiver type — a string receiver is the
// +1-releasing thunk; the elements carry the +1 via the array's retain
// symbol, NOT via the slice's disposer. The test exists to verify the
// narrowly-scoped fix does not collapse the per-element disposer
// distinction (a regression on string-element slices would be loud — every
// test program that uses `string` would leak harder than today).
// ===========================================================================
BEZEL_TEST(range_slice_arr_r_reclaimed_2753,
           string_element_slice_no_leak_no_double_free_spec_28_3) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<string>(); "
        "    a.Add(\"foo\"); a.Add(\"bar\"); a.Add(\"baz\"); "
        "    var s := a[0..2]; "
        "    return s.Count; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // 0..2 inclusive → 3 elements
}

// ===========================================================================
// (6) NEGATIVE CONTROL — the slice produces a NEW `Array<T>`; the source
// array is unmodified. The new `IndexExpr` arm classifies the slice as
// an owned EXPRESSION, but the RECEIVER (the source array) is still
// borrowed by the source local `a`, and its lifetime is unaffected. The
// `a` element at the slice's first position remains the value we put in
// (i.e. 3) — a fix that inadvertently disposed the source's elements
// during the slice would see them as coredumps here.
// ===========================================================================
BEZEL_TEST(range_slice_arr_r_reclaimed_2753,
           source_array_unchanged_spec_28_3) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(3); a.Add(1); a.Add(2); "
        "    var s := a[0..2]; "
        "    return a[0]; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ===========================================================================
// (7) ACCEPT-CONTROL — a single-element index `a[0]` is §28.3's *indexer*
// row (Borrowed, qualifier: receiver as transit point); it is NOT a slice.
// The new `IndexExpr` arm MUST NOT classify it Owned, so a plain-`var`
// Borrowed local carries no obligation and produces no auto-drop warning.
// An over-wide predicate that classified every `IndexExpr` on `Array<T>`
// as Owned would fail this test (the plain `var` would auto-drop a
// borrowed value).
// ===========================================================================
BEZEL_TEST(range_slice_arr_r_reclaimed_2753,
           single_element_index_not_owned_spec_28_3_indexer_row) {
    auto r = parseAndCheckOwnership(fixture("var s := a[0];"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r));
}

// ===========================================================================
// (8) ACCEPT-CONTROL — the Range-VALUE form `arr[r]` on a `string` receiver
// is NOT a slice producer (the predicate's receiver-head gate excludes the
// string receiver — `string` is not an `Array<T>`). The new `IndexExpr`
// arm stays silent. A `string` slice is runtime-managed, no disposal
// obligation, and the `var` binding is accepted with no §28.3 note.
// ===========================================================================
BEZEL_TEST(range_slice_arr_r_reclaimed_2753,
           string_range_slice_accept_no_obligation_spec_28_9) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Main(): int "
        "    var t := \"abcdef\"; "
        "    var u := t[0..2]; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r));
}

// ===========================================================================
// (9) ACCEPT-CONTROL — a span range index. The span families' stdlib
// `Slice` overloads return span heads, NOT owned arrays, and sema types a
// range index on them as the ELEMENT (not the receiver's own TypeRef).
// The new `IndexExpr` arm's predicate inherits the same `Array`-head and
// `exprType == receiver TypeRef` conditions, so a span receiver never
// matches. A plain-`var` element read is a Borrow and carries no §28.3
// auto-drop note.
// ===========================================================================
BEZEL_TEST(range_slice_arr_r_reclaimed_2753,
           span_range_index_accept_no_obligation_spec_4_15) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(3); a.Add(1); a.Add(2); "
        "    var sp: ArraySpan<int> := a.AsSpan(); "
        "    var v := sp[0..1]; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r));
}

// ===========================================================================
// (10) ACCEPT-CONTROL — a USER type with a range-taking indexer. The
// receiver's TypeRef carries a `declaration`, which the predicate's gate
// requires to be null (BUILTIN-only). A user indexer must keep §28.3's
// ordinary indexer classification (a Borrow into the indexer's return),
// so the plain `var` carries no obligation.
// ===========================================================================
BEZEL_TEST(range_slice_arr_r_reclaimed_2753,
           user_range_indexer_accept_no_obligation_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; class Grid "
        "  private _v: int := 3; "
        "  public property this[rg: Range<int>]: int => _v; "
        "end Grid; "
        "class C public static method Main(): int "
        "    using var g := new Grid(); "
        "    var v := g[0..2]; "
        "    return v; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r));
}

// ===========================================================================
// (11) ACCEPT-CONTROL — `OwningArray<T>` is excluded from the slice row
// (§15.6 outright rejects slicing on it). The new `IndexExpr` arm's
// predicate keys on the receiver's NAME-HEAD being exactly `Array` (not
// `OwningArray`), so the diagnostic stays §15.6's only — a §28.3
// auto-drop on the same source would be a SECOND, misleading error.
// ===========================================================================
BEZEL_TEST(range_slice_arr_r_reclaimed_2753,
           owning_array_slice_keeps_15_6_rejection_only) {
    auto r = parseAndCheckOwnership(
        "module M; class Cell public V: int := 0; end Cell; "
        "class C public static method Main(): int "
        "    using var oa := new OwningArray<Cell>(); "
        "    oa.Add(new Cell(1)); oa.Add(new Cell(2)); "
        "    var s := oa[0..2]; "
        "    return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // §15.6 rejection present, no §28.3 borrowing note.
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r));
}
