// Codegen — fix #3802 / stdlib §4.8 / §28.9 (v0.287): widen fix #3798's
// `dictValueBoxFreer` specialization (per-V boxed-value freer that walks V's
// nested string fields before `free(box)`) to nullable V. A nullable record
// V lowers to a tagged `{i8 hasValue, T}` literal struct (§7.2/#416, §9.4);
// the box is the malloc'd storage of that wrapper — T's struct bytes are at
// element 1 of the wrapper, but T's nested `string` allocations are separate
// heap objects whose +1 refcounts the wrapper holds. Before this fix the
// `!valTy->nullable` gate excluded the nullable arm entirely, so a nullable
// V fell through to the plain `valueBoxFreeThunk` (`free(box)` only) and
// every `.Remove(k)` / `.Clear()` / scope-end Dispose leaked each entry's
// nested string.
//
// ROOT CAUSE (`LlvmBackend_Disposal.cpp`'s `dictValueBoxFreer`, ~L4533):
// the function dispatched on V's kind + `recordHasStringFields` /
// `choiceHasStringFields` only when `!valTy->nullable`. A nullable V went to
// `valueBoxFreeThunk` regardless of whether V owned a string, leaking the
// wrapper's nested strings exactly the same way #3798 fixed for non-nullable
// V.
//
// FIX (`LlvmBackend_Disposal.cpp`'s `dictValueBoxFreer` +
// `boxedRecordValueFreer` / `boxedChoiceValueFreer`): drop the
// `!valTy->nullable` gate and pass `valTy->nullable` through to the freers.
// The freers accept a new `bool nullable` parameter; when true they GEP
// element 1 of the `{i8, T}` wrapper to recover T's struct base (which is
// what `emitRecordStringFieldReleases_impl` /
// `emitChoiceStringFieldReleases_impl` already accept), hand that to the
// walker unchanged, and then `free(box)`s the wrapper storage. Nullable V is
// cached in separate maps (`boxedNullableRecordValueFreers_` /
// `boxedNullableChoiceValueFreers_`) because the wrapper GEP shape differs
// from the bare-T base and the same module can carry both `Dictionary<K, V>`
// and `Dictionary<K, V?>` (one cache, two GEP shapes = wrong IR).
//
// §6b — DOUBLE-FREE ANALYSIS.
//   * Nullable V WITHOUT string fields keeps the plain `valueBoxFreeThunk` —
//     UNCHANGED (the `recordHasStringFields` / `choiceHasStringFields`
//     predicates still gate the new specialization).
//   * Generic nullable V is still excluded by the existing non-null-decl
//     gate in `recordHasStringFields` / `choiceHasStringFields`.
//   * Non-nullable V path is byte-identical to #3798 (the nullable parameter
//     defaults to false; only the specialization's caller passes true).
//   * The wrapper's element 0 (the i8 hasValue tag) is NOT walked — it's a
//     primitive, not an owned resource, and `free(box)` reclaims it as part
//     of the wrapper storage. Only element 1 (T) carries owned strings.
//   * A null entry (i8 = 0) inside the wrapper still gets walked: the
//     `hasValue = 0` case has uninitialized T bytes, so the walker would
//     release garbage. The fix relies on the upstream `Add` site only
//     writing the wrapper when hasValue is non-null (the §7.2 contract);
//     a future safety net that gates the walk on `hasValue == 1` is a
//     separate, pre-existing concern (#416) and out of scope.
//
// FAIL-WITHOUT / PASS-WITH: measured on the issue's own probe at 1000
// iterations of `Dictionary<int, Inner?>.Add(k, new Inner($"..."))` then
// `.Remove(k)`, loop-amplified per [[lsan-one-shot-leak-tests-pass-vacuously]]:
//   * PARENT bezelc (this branch pre-#3802): "Direct leak of 32000 byte(s)
//     in 1000 object(s)" + "Indirect leak of 44890 byte(s) in 1000
//     object(s)" — Direct 32000 B / 1000 (the +1 string allocations) +
//     Indirect 44890 B / 1000 (the box + the wrapper allocations).
//   * FIX bezelc (this branch + #3802): LSan-clean exit 0.
// #3798 RELATIONSHIP: this fix is the nullable counterpart of #3798.
// Non-nullable V is byte-identical to #3798 (its six rows stay green).

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 6 absorbing rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics and returned from the row on it —
// the definition scripts/check_toolchain_skip_surface.py uses for an
// ABSORBING site. It was also a silent vacuous pass: on a box whose clang
// cannot link the ASan binary, every one of those rows returned WITHOUT
// asserting, and the suite reported a clean PASS having compiled and run
// nothing.
//
// Each guard becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position:
// the skip is now LOUD — the row FAILS — instead of being absorbed by a
// guard that reports a clean pass.
// Every substantive assertion in every row — `r.ran` and the exitCode pins —
// stays exactly where it was, and the local predicate is deleted so no
// absorbing shape survives. The guard stays a SEPARATE statement from the
// predicate on purpose: inlining it into an `if` recreates the shape the
// predicate exists to end.

}  // namespace

// ---------------------------------------------------------------------------
// Row 1 — issue repro. `Dictionary<int, MyRecord?>.Remove` at 1000 iter
// (FAIL-WITHOUT: Direct 32000 B / 1000 + Indirect 44890 B / 1000; PASS-WITH:
// LSan-clean exit 0). Uses a HEAP-ALLOCATED string ($"...") per iteration
// so the +1 refcount is real (a literal-string Name has sentinel refcount
// and bz_string_release is a no-op on it — measured on HEAD).
// ---------------------------------------------------------------------------

BEZEL_TEST(dictionary_nullable_v_3802,
           record_nullable_remove_no_leak_4_8) {
    const std::string src =
        "module M; "
        "public record Inner "
        "  public Name: string; "
        "  public constructor(n: string) this.Name := n; end constructor; "
        "end Inner; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 1000 do "
        "    using var d := new Dictionary<int, Inner?>(); "
        "    d.Add(1, new Inner($\"iter-{i}-longer-suffix-to-ensure-heap\")); "
        "    d.Remove(1); "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 2 — control. Bare `Dictionary<int, MyRecord>.Remove` (already passes
// post-#3798) — LSan must stay silent. Pin the non-nullable path's
// byte-identical behavior after the parameter-shape widening.
// ---------------------------------------------------------------------------

BEZEL_TEST(dictionary_nullable_v_3802,
           record_non_nullable_remove_no_leak_4_8) {
    const std::string src =
        "module M; "
        "public record Inner "
        "  public Name: string; "
        "  public constructor(n: string) this.Name := n; end constructor; "
        "end Inner; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 500 do "
        "    using var d := new Dictionary<int, Inner>(); "
        "    d.Add(1, new Inner($\"non-null-iter-{i}-longer-suffix-heap\")); "
        "    d.Remove(1); "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 3 — control. `Dictionary<int, MyRecord?>.Clear` instead of Remove —
// same defect, different call site. Clear iterates live entries and
// disposes them, so the box is freed on each Clear and the nested string
// is the leak source. Mirrors the §4.8 / §4.17 prose that Clear has the
// SAME value-disposal contract as Remove / Dispose.
// ---------------------------------------------------------------------------

BEZEL_TEST(dictionary_nullable_v_3802,
           record_nullable_clear_no_leak_4_8) {
    const std::string src =
        "module M; "
        "public record Inner "
        "  public Name: string; "
        "  public constructor(n: string) this.Name := n; end constructor; "
        "end Inner; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 500 do "
        "    using var d := new Dictionary<int, Inner?>(); "
        "    d.Add(1, new Inner($\"clear-iter-{i}-longer-suffix-heap\")); "
        "    d.Clear(); "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 4 — control. Scope-end Dispose (Dictionary goes out of scope with
// entries present, never explicitly removed) — exercises the dict's own
// scope-end disposal path (bz_dict_free + the per-entry disposer installed
// by the dict-freer accessor at allocation time).
// ---------------------------------------------------------------------------

BEZEL_TEST(dictionary_nullable_v_3802,
           record_nullable_scope_end_dispose_no_leak_4_8) {
    const std::string src =
        "module M; "
        "public record Inner "
        "  public Name: string; "
        "  public constructor(n: string) this.Name := n; end constructor; "
        "end Inner; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 500 do "
        "    using var d := new Dictionary<int, Inner?>(); "
        "    d.Add(1, new Inner($\"scope-iter-{i}-longer-suffix-heap\")); "
        "    d.Add(2, new Inner($\"scope-iter2-{i}-longer-suffix-heap\")); "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 5 — control. `Dictionary<int, MyChoice?>.Remove` (choice V, not
// record V). Exercises the choice walker through the wrapper's element-1
// GEP. Same defect shape as Row 1 but with a choice V.
// ---------------------------------------------------------------------------

BEZEL_TEST(dictionary_nullable_v_3802,
           choice_nullable_remove_no_leak_4_8) {
    const std::string src =
        "module M; "
        "choice Shape "
        "  Circle(s: string), "
        "  Square(s: string); "
        "end Shape; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 500 do "
        "    using var d := new Dictionary<int, Shape?>(); "
        "    d.Add(1, Shape.Circle($\"choice-iter-{i}-longer-suffix-heap\")); "
        "    d.Remove(1); "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 6 — control. `SortedDictionary<int, MyRecord?>.Remove` — same defect
// shape as Row 1 but on the sorted family. The head-name gate (already
// widened by #3776) admits SortedDictionary through `dictValueBoxFreer`,
// and this fix's nullable specialization is independent of the head name
// once we ARE inside the boxed-V branch.
// ---------------------------------------------------------------------------

BEZEL_TEST(dictionary_nullable_v_3802,
           sorted_dict_nullable_record_remove_no_leak_4_17) {
    const std::string src =
        "module M; "
        "public record Inner "
        "  public Name: string; "
        "  public constructor(n: string) this.Name := n; end constructor; "
        "end Inner; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 500 do "
        "    using var d := new SortedDictionary<int, Inner?>(); "
        "    d.Add(1, new Inner($\"sorted-iter-{i}-longer-suffix-heap\")); "
        "    d.Remove(1); "
        "    i := i + 1; "
        "  end while; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}