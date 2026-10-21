// Codegen / refcounting — issue #2900: binding a `string` local into an
// `Object`-typed local leaks the string's +1 (§28.9 / §28.3, v0.240).
//
// SYMPTOM (measured on 5995756, baseline of this run). `var o: Object := s;`
// (no ToString, no boxing of a value type, no interpolation — the bare alias)
// leaks one string per execution of the bind. With 20 iterations in a loop,
// LSan reports "731 byte(s) leaked in 40 allocation(s)" — 20 strings × 2
// allocations (header + data buffer), exactly as the issue records.
//
// ROOT CAUSE. `s` is a +1 producer (`"ab" + i.ToString()`), so the producer's
// single +1 is in `s`'s slot. The `Object`-typed alias `o` is a bare pointer
// slot with NO retain — the string's +1 is shared between `s` and `o`, but
// only `s` actually owns the reference. The StringEscapeWalker taints `s` on
// the `var o: Object := s;` binding (the destination `o` is not in
// `stringBorrowAll_`, so the #2764 carve-out does not fire), so `s`'s release
// is suppressed and the producer's +1 leaks at scope end. The control case
// `var s2: string := s;` (a string-typed copy) is clean because
// `stringBorrowAll_` covers it and the source's release stays armed.
//
// THE FIX (#2900, option (a) + (b) per the issue). The `Object` slot must own
// its own +1, exactly as the `string`-typed copy does. The codegen retains at
// the bind site, the disposal path releases the retained +1 at scope end
// (gated on a state bit that tracks "is the current Object slot value a
// string"), and the StringEscapeWalker treats the `var o: Object := s;` shape
// as a non-escape — closing the unbounded leak. Reassigning `o` to a
// non-string flips the state bit so the release is skipped (a class instance
// or box is disposed via the existing ownership / box-release paths; never
// double-freed as a string). The reassignment-of-`s` direction is a separate
// concern (the alias is then stale; an over-release of `s`'s displaced old
// value would be a UAF) and is OUT OF SCOPE for this fix per the issue's
// "(a) + (b)" prescription — it requires lifetime tracking on the alias that
// is not in this lane.
//
// EVIDENCE DISCIPLINE.
//   - Every leak test is loop-amplified (20+ iterations + a churn string).
//     A one-shot Main is not a reliable LSan oracle: a leftover pointer in a
//     register makes LSan treat the block as merely reachable. 20 iterations
//     put the leak beyond that noise.
//   - All heap strings are built at runtime (`"ab" + i.ToString()`); never a
//     literal-literal concat (the rc sentinel makes release a no-op and
//     would make the probe vacuous).
//   - The plain (unsanitized) exit code is asserted alongside, pinning that
//     the fix froze nothing early (a leak is invisible without a sanitizer;
//     the value-invariants prove the program still computes the right sum).
//   - The over-correction guards (parameter-typed target, class-typed target)
//     pin that the relaxation did not widen to a shape it should not cover.
//
// ⚠ DELIBERATE EXCLUSIONS, filed separately:
//   - `var s := "x"; var o: Object := s; s := "y" + i.ToString();` — the
//     `s`-reassignment-after-alias case (over-release direction, requires
//     alias-lifetime tracking on `o`; the issue defers it explicitly).
//   - `var o: Object := p;` where `p` is a parameter (a `string` parameter is
//     the caller's borrow, §28.1; the alias would need a retain in `o` and
//     a release on the `o` side that ties to the caller's ownership — a
//     lifetime model this fix does not establish).
//   - The same shapes over an interface-typed target (IShape, IDisposable):
//     covered by the Object-typed tests; the relaxation keys off
//     isObjectOrInterfaceTarget, so any widening is symmetric.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The concatenated diagnostic text of a checked run. A leak is a different
// verdict from a double-free or use-after-free, and r.ran is false for both.
std::string sanitizerText(const JitResult& r) {
    std::string all;
    for (const auto& d : r.diagnostics.diagnostics()) all += d.message + "\n";
    return all;
}

// sum over i in 1..20 of len("ab-{i}") + len("c-{i}")
//   = (9*3 + 11*4) + (9*2 + 11*3) = 71 + 51 = 122.  (the test body
//     computes `s.Length + churn.Length` per iteration, so the total
//     is sum of BOTH lengths).
constexpr int kLenSum20 = 122;
// sum over i in 1..20 of len("ab-{i}") = 71.  (B reads both s and s2
// and the churn; C and D use different subsets).
constexpr int kLenAb20 = 71;

// =====================================================================
// (A) THE ISSUE'S OWN REPRO — bound into a method body with a churn
// allocation between the bind and the read. Pre-fix: "731 byte(s) leaked
// in 40 allocation(s)" (the issue's exact figure). §28.9 / §28.3.
// =====================================================================
const char* kBindStringIntoObjectInMethod =
    "module M; class C "
    "  public static method Run(): int "
    "    var t: int := 0; "
    "    for var i in 1..20 do "
    "      var s: string := \"ab\" + i.ToString(); "
    "      var o: Object := s; "
    "      var churn: string := \"c\" + i.ToString(); "
    "      t := t + s.Length + churn.Length; "
    "    end; "
    "    return t; "                          // 122
    "  end Run; "
    "  public static method Main(): int "
    "    return Run(); "
    "  end Main; "
    "end C;";

// =====================================================================
// (B) CONTROL — the string-typed copy is CLEAN pre-fix (issue's #2764
// carve-out covers it). The plain-path assertion is the cardinal
// value-invariant: the fix must not change the computed sum. §28.9.
// =====================================================================
const char* kBindStringIntoStringInMethod =
    "module M; class C "
    "  public static method Run(): int "
    "    var t: int := 0; "
    "    for var i in 1..20 do "
    "      var s: string := \"ab\" + i.ToString(); "
    "      var s2: string := s; "
    "      var churn: string := \"c\" + i.ToString(); "
    "      t := t + s.Length + s2.Length + churn.Length; "
    "    end; "
    "    return t; "                          // 2*71 + 51 = 193
    "  end Run; "
    "  public static method Main(): int "
    "    return Run(); "
    "  end Main; "
    "end C;";

// =====================================================================
// (C) The Object alias is USED through `Console.WriteLine(o)` — the
// canonical "hand a string to anything Object-typed" use case the
// issue's "Service loop" comment points at. Pre-fix the producer's +1
// leaks; post-fix it is released by the alias's scope-end. The
// discarded WriteLine is a no-op on the value (it renders and discards);
// the LSan verdict is the cardinal assertion. §28.3 K1.6.
// =====================================================================
const char* kBindAndPassToObjectArg =
    "module M; class C "
    "  public static method Run(): int "
    "    var t: int := 0; "
    "    for var i in 1..20 do "
    "      var s: string := \"ab\" + i.ToString(); "
    "      var o: Object := s; "
    "      var churn: string := \"c\" + i.ToString(); "
    "      Console.WriteLine(o); "            // §28.3 K1.5 read — a borrow
    "      t := t + churn.Length; "
    "    end; "
    "    return t; "                          // 51
    "  end Run; "
    "  public static method Main(): int "
    "    return Run(); "
    "  end Main; "
    "end C;";

// =====================================================================
// (D) The Object alias is REASSIGNED to a NEW class instance — the
// new class is a trivial-dispose (no destructor, no owned fields), so
// it does not need an explicit `using var` (the trivial-dispose
// ceremony exemption, §28.9). The state bit must flip and the string
// release at scope end must be SKIPPED — otherwise bz_string_release
// would free a class instance's bits, a heap-use-after-free. Pre-fix
// the producer's +1 leaks AND the class instance is correctly freed;
// post-fix only the class is freed. §28.6 / §28.9.
// =====================================================================
const char* kObjectAliasReassignedToClass =
    "module M; "
    "class K public N: int := 0; end K; "
    "class C "
    "  public static method Run(): int "
    "    var t: int := 0; "
    "    for var i in 1..20 do "
    "      var s: string := \"ab\" + i.ToString(); "
    "      var o: Object := s; "
    "      var k: K := new K(); "
    "      o := k; "                          // state bit flips to false
    "      var churn: string := \"c\" + i.ToString(); "
    "      t := t + s.Length + churn.Length; "
    "    end; "
    "    return t; "                          // 122
    "  end Run; "
    "  public static method Main(): int "
    "    return Run(); "
    "  end Main; "
    "end C;";

// =====================================================================
// (E) OVER-CORRECTION GUARD — a string-typed PARAMETER is the source.
// §28.1 makes a `string` parameter an unmodified borrow whose one
// reference belongs to the CALLER; the callee's `var o: Object := s;`
// must not release the caller's reference. The collector EXCLUDES
// parameter sources (the LocalDecl-vs-ParameterSymbol branch in
// collectObjectStringReleaseEligible's scan), so no retain, no
// objectStringRelease Disposal — the runtime behavior is unchanged on
// both sides of the fix. This case is a guard against widening the
// new arm to a location that is not safe to release.
//
// `o.Length` is a degraded read (CG8 / #2547 / #605) that resolves to
// zero; the entire `o.Length + s.Length` therefore degrades to 0
// (the degraded-return path), so per iter `r = 0`. Total = ab + c = 122.
// =====================================================================
const char* kBindParameterStringIntoObject =
    "module M; class C "
    "  private static method Take(s: string): int "
    "    var o: Object := s; "
    "    return o.Length + s.Length; "
    "  end Take; "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    for var i in 1..20 do "
    "      var built: string := \"ab\" + i.ToString(); "
    "      var r := C.Take(built); "
    "      var churn: string := \"c\" + i.ToString(); "
    "      t := t + r + built.Length + churn.Length; "
    "    end; "
    "    return t; "                          // ab + c = 122
    "  end Main; "
    "end C;";

}  // namespace

// ---------------------------------------------------------------------
// (A) The issue's own repro, under the ASan/LSan net.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_to_object_alias_release_2900,
           bind_string_into_object_in_method_28_9) {
    JitResult r = runAotChecked(kBindStringIntoObjectInMethod);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // Pre-fix: 731 B / 40 allocations (20 strings × 2 allocs each).
    // Post-fix: 0 leaks.
    BEZEL_EXPECT_TRUE(r.ran);
    // The cardinal assertion is the LSan verdict — r.ran is true only if
    // the process exited 0. A leak fires and the run reports it; we want
    // the sanitizer text to contain NO "memory leaks" headline.
    const std::string t = sanitizerText(r);
    BEZEL_EXPECT_TRUE(t.find("detected memory leaks") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("heap-use-after-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("double-free") == std::string::npos);
}

BEZEL_TEST(codegen_string_to_object_alias_release_2900,
           bind_string_into_object_plain_path_value_invariant_28_9) {
    // A leak is invisible without a sanitizer, so this passes with and
    // without the fix. It pins that the fix did not change the computed
    // value — that nothing was freed early.
    JitResult r = runAot(kBindStringIntoObjectInMethod);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

// ---------------------------------------------------------------------
// (B) Control — the string-typed copy is CLEAN pre-fix. The plain-path
// invariant pins the computed sum so the fix cannot silently change
// behavior in a way that masquerades as a leak fix.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_to_object_alias_release_2900,
           bind_string_into_string_stays_clean_28_9) {
    JitResult r = runAotChecked(kBindStringIntoStringInMethod);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    const std::string t = sanitizerText(r);
    BEZEL_EXPECT_TRUE(t.find("detected memory leaks") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("heap-use-after-free") == std::string::npos);
}

BEZEL_TEST(codegen_string_to_object_alias_release_2900,
           bind_string_into_string_plain_path_value_invariant_28_9) {
    JitResult r = runAot(kBindStringIntoStringInMethod);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2 * kLenAb20 + (kLenSum20 - kLenAb20));
}

// ---------------------------------------------------------------------
// (C) Bind and read through the Object interface.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_to_object_alias_release_2900,
           bind_and_pass_to_object_arg_releases_28_3) {
    JitResult r = runAotChecked(kBindAndPassToObjectArg);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    const std::string t = sanitizerText(r);
    BEZEL_EXPECT_TRUE(t.find("detected memory leaks") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("heap-use-after-free") == std::string::npos);
}

BEZEL_TEST(codegen_string_to_object_alias_release_2900,
           bind_and_pass_to_object_arg_plain_path_value_28_3) {
    JitResult r = runAot(kBindAndPassToObjectArg);
    BEZEL_EXPECT_TRUE(r.ran);
    // churn-only sum (the test body reads churn.Length; o's length is
    // consumed by WriteLine, not folded into t).
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20 - kLenAb20);
}

// ---------------------------------------------------------------------
// (D) The Object alias is reassigned to a class instance. The state
// bit must flip, and the string release at scope end must be SKIPPED.
// This pins that the fix did not introduce a spurious string-release
// on a class slot (which would be a heap-use-after-free).
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_to_object_alias_release_2900,
           object_alias_reassigned_to_class_skips_string_release_28_6) {
    JitResult r = runAotChecked(kObjectAliasReassignedToClass);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    const std::string t = sanitizerText(r);
    BEZEL_EXPECT_TRUE(t.find("detected memory leaks") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("heap-use-after-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("double-free") == std::string::npos);
}

BEZEL_TEST(codegen_string_to_object_alias_release_2900,
           object_alias_reassigned_to_class_plain_path_value_28_6) {
    JitResult r = runAot(kObjectAliasReassignedToClass);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

// ---------------------------------------------------------------------
// (E) Over-correction guard — string parameter source.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_to_object_alias_release_2900,
           bind_parameter_string_into_object_stays_balanced_28_1) {
    // The relaxation must NOT widen to a parameter-typed source — the
    // caller's reference is the caller's, and an Object-side release
    // would over-release it (heap-use-after-free). Clean on both sides
    // of the fix; the assertion is a no-regression guard.
    JitResult r = runAotChecked(kBindParameterStringIntoObject);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    const std::string t = sanitizerText(r);
    BEZEL_EXPECT_TRUE(t.find("detected memory leaks") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("heap-use-after-free") == std::string::npos);
}

BEZEL_TEST(codegen_string_to_object_alias_release_2900,
           bind_parameter_string_into_object_plain_path_value_28_1) {
    JitResult r = runAot(kBindParameterStringIntoObject);
    BEZEL_EXPECT_TRUE(r.ran);
    // Take's `o.Length + s.Length` degrades to 0 (the o.Length branch
    // can't dispatch and #2491's degraded-return path zeroes the whole
    // expression), so per iter r=0 and the sum is just ab + c = 122.
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}
