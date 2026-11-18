// Ownership / codegen leak — fix #8817. A closure stored into a `scoped` field of a
// class was never freed: the producer's reference was dropped by nobody, so
// every store leaked the 64-byte closure cell. The stored closure is owned
// storage of the holder (§28.14's "storage-bounded" bound, §28.9's
// runtime-managed closure cell), and must be released exactly once — when the
// field is overwritten, or when the holder is disposed.
//
// Spec: §28.14 (scoped callback parameters — storage-bounded capture; "The
// callee may store the callback, but only in `scoped` fields of the bounding
// object itself"), §28.9 (runtime-managed types — "Runtime management governs
// the function value itself — the heap-allocated closure object that backs
// it"), §28.3 (local variables, ownership and borrows — a by-value
// function-typed parameter is an unmodified BORROW), §27.5 (closures).
// Spec revision v0.359.
//
// THE DEFECT (issue #8817's measured repro, verbatim). `h.Reg(lambda() => 7)`
// passes a `+1` closure producer to a `scoped` parameter; the callee stores it
// into its own `scoped` field. The callee's store (`lowerClosureFieldStore`)
// RETAINS the value for the field — a class closure field holds an
// INDEPENDENT +1, retained at store and released on the owner's disposal —
// so the caller's producer reference was still the caller's to drop after the
// call. The codegen's closure-argument discharge did not drop it: the
// `scoped` arm of `prepareCallClosureArgRelease` armed only for a callee
// whose body never REFERENCES the parameter (the #3195 "SCOPED PASSIVE" case),
// on the (wrong) theory that a referencing callee might store the callback
// without taking a reference. It does take one. So the producer's +1 was
// dropped by nobody: 64 B per store, 12800 B in 200 allocations, LSan-
// confirmed on the base of this branch.
//
// MEASURED on this branch's base (r30-c1 @ 0d5d80a58):
//   Direct leak of 12800 byte(s) in 200 object(s).
//   SUMMARY: AddressSanitizer: 12800 byte(s) leaked in 200 allocation(s).
// After the fix: clean (exit 0, no sanitizer report).
//
// WHY THE CALLER MAY DROP IT (§28.14 / §28.9). A `scoped` formal is received
// as a BORROW (§28.3: a by-value function-typed parameter is an unmodified
// borrow; §28.1 makes `owned` inadmissible on a function type). §28.14
// enumerates the callee's three permitted uses — invoke it, store it into a
// `scoped` field of the bounding object, or forward it to a `scoped` parameter
// of a call on `this` — and sema enforces the list (a `scoped` formal may not
// be bound to a local, returned, or stored anywhere else). Of those three:
// the invocation takes no reference (the caller's +1 is the only one); the
// store RETAINS for the field (so the field owns a reference of its own, and
// the field's reference is dropped exactly once — by the store's release-old
// on overwrite, or by the owner's disposer when the holder goes away); the
// forwarding hands the callee a value that is not a `+1` producer, so no
// release is armed for it. So the caller's producer reference is the
// CALLER's to drop on every `scoped` formal — the same discipline the string
// argument-pass release already follows for every formal (§28.9: "the callee
// receives the string as a BORROW … if the callee stores the param into a
// string field the field-store path inside the callee already retains it
// independently (rc→2), so our release here (rc→1) is safe").
//
// ROWS. A, B and C are RED on base (the leak sentinel fires) and GREEN with
// the fix. D and E are the two BOUNDARY CONTROLS of this suite: base-green
// programs whose verdict the corpus cannot express (a runtime use-after-free /
// double-free check), each going RED under a plausible wrong fix — see the
// commit body.
//
// The programs are the issue's own repro and its measured siblings; every
// leak repro loops 200× so the leak cannot hide under the sanitizer's noise.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

}  // namespace

// ===========================================================================
// A — THE ISSUE'S OWN REPRO (RED on base, GREEN with the fix). A closure
// stored into a `scoped` field of a holder that then goes away: the stored
// closure is owned storage of the holder and must be released when the
// holder is disposed (§28.14's storage-bounded bound, §28.9's runtime-
// managed closure cell). On base this leaks the 64-byte closure cell per
// store — 12800 B in 200 allocations, LSan-confirmed.
// ===========================================================================
BEZEL_TEST(ownership_8817_scoped_field_closure,
           stored_closure_released_when_holder_goes_away_spec_28_14) {
    auto r = runAotChecked(
        "module P; "
        "public class Holder "
        "  private scoped _cb: (() -> int)?; "
        "  public method Reg(scoped f: () -> int) "
        "    _cb := f; "
        "  end Reg; "
        "  public method Ignore(scoped f: () -> int): int => 1; "
        "  public method Fire(): int => _cb?() ?? -5; "
        "end Holder; "
        "public class Program "
        "  public static method Main() "
        "    var n := 0; var i := 0; "
        "    while i < 200 do "
        "      var h := new Holder(); "
        "      h.Reg(lambda() => 7); "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(n); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// B — OVERWRITE (RED on base, GREEN with the fix). Two stores into the SAME
// `scoped` field: the first store's closure is DISPLACED, and the displaced
// closure must be released exactly once — by the second store's release-old
// (§28.7's drop-on-overwrite, applied to the field's own reference) — while
// the second store's closure is released when the holder is disposed. Two
// stores, so two cells on base: 25600 B in 400 allocations, LSan-confirmed.
// A wrong fix that released the displaced value twice, or that let the
// holder's disposer release it again, is a double free and turns this row
// red under ASan.
// ===========================================================================
BEZEL_TEST(ownership_8817_scoped_field_closure,
           displaced_closure_released_exactly_once_on_overwrite_spec_28_14) {
    auto r = runAotChecked(
        "module P; "
        "public class Holder "
        "  private scoped _cb: (() -> int)?; "
        "  public method Reg(scoped f: () -> int) "
        "    _cb := f; "
        "  end Reg; "
        "  public method Fire(): int => _cb?() ?? -5; "
        "end Holder; "
        "public class Program "
        "  public static method Main() "
        "    var n := 0; var i := 0; "
        "    while i < 200 do "
        "      var h := new Holder(); "
        "      h.Reg(lambda() => 1); "
        "      h.Reg(lambda() => 2); "
        "      n := h.Fire(); "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(n); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// C — CAPTURE-FREE vs CAPTURING (RED on base, GREEN with the fix). The same
// `scoped` field, filled first with a capture-free closure and then with a
// CAPTURING one: the capturing closure's env block and its capture cell must
// be reclaimed with the closure (§28.9: the closure cell is runtime-managed;
// §27.5: the capture cell's lifetime is the closure's). On base the two
// stores leak 28000 B in 800 allocations (2 cells + 1 env + 1 capture cell
// per iteration), LSan-confirmed.
// ===========================================================================
BEZEL_TEST(ownership_8817_scoped_field_closure,
           capturing_closure_released_with_its_capture_cell_spec_27_5) {
    auto r = runAotChecked(
        "module P; "
        "public class Holder "
        "  private scoped _cb: (() -> int)?; "
        "  public method Reg(scoped f: () -> int) "
        "    _cb := f; "
        "  end Reg; "
        "  public method Fire(): int => _cb?() ?? -5; "
        "end Holder; "
        "public class Program "
        "  public static method Main() "
        "    var n := 0; var i := 0; "
        "    while i < 200 do "
        "      var h := new Holder(); "
        "      var k := 5; "
        "      h.Reg(lambda() => 7); "
        "      h.Reg(lambda() => k); "
        "      n := h.Fire(); "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(n); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// D — BOUNDARY CONTROL #1: ONE CLOSURE SHARED BY TWO HOLDERS. The same
// closure value is stored into two different holders' `scoped` fields. Each
// field holds its OWN +1 (retained at store), so the cell survives until
// BOTH holders are disposed and is freed exactly once. This program leaks the
// LOCAL's own reference (a closure local that escapes is conservatively left
// leaked — a separate, pre-existing gap this lane does not touch), so the
// leak sentinel is disabled and the row checks the USE-AFTER-FREE /
// DOUBLE-FREE direction only — a check the conformance corpus cannot
// express (it decides compile-time verdicts). It goes RED under a plausible
// wrong fix: one that TRANSFERS the caller's reference into the field instead
// of retaining an independent one (the second field would then own a
// reference nobody holds, and the first holder's disposal would free the
// cell under the second).
// ===========================================================================
BEZEL_TEST(ownership_8817_scoped_field_closure,
           shared_closure_stored_in_two_holders_no_double_free_spec_28_9) {
    auto r = runAotCheckedLenient(
        "module P; "
        "public class Holder "
        "  private scoped _cb: (() -> int)?; "
        "  public method Reg(scoped f: () -> int) "
        "    _cb := f; "
        "  end Reg; "
        "  public method Fire(): int => _cb?() ?? -5; "
        "end Holder; "
        "public class Program "
        "  public static method Main() "
        "    var n := 0; var i := 0; "
        "    while i < 200 do "
        "      var h1 := new Holder(); "
        "      var h2 := new Holder(); "
        "      var g := lambda() => 7; "
        "      h1.Reg(g); "
        "      h2.Reg(g); "
        "      n := h1.Fire() + h2.Fire(); "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(n); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// E — BOUNDARY CONTROL #2: THE CLOSURE IS ALSO HELD BY A LOCAL. The local
// owns the producer's +1; the holder's field takes an INDEPENDENT one, so the
// cell survives the local's death and is freed once, by the holder's
// disposal. Same shape as D: the LOCAL's own reference is left leaked (the
// pre-existing closure-local escape gap), so the leak sentinel is off and
// the row checks the USE-AFTER-FREE / DOUBLE-FREE direction — a check the
// conformance corpus cannot express. It goes RED under a plausible wrong
// fix: one that drops the caller's producer reference for an argument that
// is a BORROW (a local) rather than a producer — the field would then hold a
// reference the caller had already given up.
// ===========================================================================
BEZEL_TEST(ownership_8817_scoped_field_closure,
           closure_also_held_by_a_local_no_use_after_free_spec_28_9) {
    auto r = runAotCheckedLenient(
        "module P; "
        "public class Holder "
        "  private scoped _cb: (() -> int)?; "
        "  public method Reg(scoped f: () -> int) "
        "    _cb := f; "
        "  end Reg; "
        "  public method Fire(): int => _cb?() ?? -5; "
        "end Holder; "
        "public class Program "
        "  public static method Main() "
        "    var n := 0; var i := 0; "
        "    while i < 200 do "
        "      var h := new Holder(); "
        "      var g := lambda() => 7; "
        "      h.Reg(g); "
        "      n := h.Fire(); "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(n); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
