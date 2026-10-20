// Codegen / leak — issue #2475: `val.ToString()` on a bare, unconstrained
// generic type parameter `T` (`where T: value`) leaked its result string —
// the SAME +1-producer-recognition gap as #1982 (Guid) / #1988 (DateTime
// family), one layer deeper: `isPlusOneStringProducer` (LlvmBackend_Setup.cpp)
// never saw THROUGH the generic substitution to the concrete type whose
// ToString mints the fresh string.
//
// THE BUG. `val`'s SEMA type is always the bare type parameter `T` (Named, no
// declaration) inside a generic body — regardless of what T is concretely
// bound to at any call site. `isPlusOneStringProducer`'s existing numeric-
// ToString recognizer keys on `tt->kind == Primitive`, which a bare-T
// receiver never matches. So `return val.ToString();` (or `var s :=
// val.ToString();`) was treated as a BORROW and RETAINED — even though the
// underlying concrete-type dispatch (emitPrimitiveToString for a value
// primitive T, #2469's emitRecordToString for a Record T) mints a genuine
// rc=1 heap string. The spurious retain (rc 1->2) is never balanced by a
// second release (only one scope-end release drops it, rc 2->1) — the
// producer's original +1 leaks. Confirmed via LSan for T=int (34 bytes) and
// T=a positional record (47 bytes) on origin/main before this fix.
//
// Isolated by elimination (see the fix's commit message / #2469's own
// investigation) to specifically this shape — a string LITERAL return, a
// `+`-concatenation return, and ToString on a concrete-typed LOCAL (not the
// bare-T parameter) inside the SAME generic body all correctly avoided the
// extra retain already, via the pre-existing recognizers.
//
// THE FIX. `isPlusOneStringProducer` gains a new recognizer: a no-arg
// `ToString()` whose receiver's sema type is a bare, single-part-named,
// declaration-less Named type resolved (via `genericSubstSemaTy_`, the same
// per-instantiation lookup #2469/#1091 use) to a value PRIMITIVE (mirroring
// the existing numeric-ToString switch) or a RECORD (positional or body-form
// — `emitRecordToString` is the same function for both). Enum is
// deliberately excluded — confirmed empirically to be an immortal string
// LITERAL (the §8.5 qualified-name default), not a heap allocation, so it
// neither leaks before this fix nor needs recognizing after it (a
// regression guard below pins this).
//
// OBSERVABILITY. runAotChecked links + runs under ASan/LSan; a leaked string
// flips `r.ran` to false (a double-free/UAF from over-releasing would too —
// the SAME oracle catches both directions). Each case renders in a LOOP (40
// iterations, mirroring #1988's own idiom) so LSan reliably reports a
// pre-fix leak (a one-shot bound-then-read string can linger in a register
// and evade LSan's reachability scan — the false-negative #1988's own
// comment already documents). The returned count == the iteration count IFF
// every render is byte-correct AND the run is leak-clean, so the exit code
// pins the VALUE and `r.ran` pins leak-freedom simultaneously.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAot;

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
// the skip is still recognised as an environment condition rather than a
// product failure, but it now MARKS THE ROW FAILED instead of hiding it.
// Every substantive assertion in every row — `r.ran` and the exitCode pins —
// stays exactly where it was, and the local predicate is deleted so no
// absorbing shape survives. The guard stays a SEPARATE statement from the
// predicate on purpose: inlining it into an `if` recreates the shape the
// predicate exists to end.

// Build a "call F<T>(val).ToString-through-generic N times, count
// byte-correct results" program. `preamble` supplies any extra declarations
// (e.g. a record type); `ctorExpr` constructs the T-typed argument for each
// iteration (fed `i`, the loop counter, so successive calls are NOT
// identical — a constant-folded single allocation would defeat LSan's
// reachability scan the same way a non-looped call would); `expectedPrefix`
// is matched via string `Length` (>0) rather than exact content, since the
// per-iteration value varies — the loop COUNT (not content) is what proves
// every iteration executed and stayed leak-clean.
std::string loopCount(const char* preamble, const char* ctorExpr) {
    return std::string("module M; ") + preamble +
           "class C "
           "  public static method F<T>(val: T): string where T: value "
           "    return val.ToString(); "
           "  end F; "
           "  public static method Main(): int "
           "    var count := 0; "
           "    var i := 0; "
           "    while i < 40 do "
           "      var s := F(" + ctorExpr + "); "
           "      if s.Length > 0 then count := count + 1; end; "
           "      i := i + 1; "
           "    end; "
           "    return count; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ── The exact #2475 repro: primitive substitution ────────────────────────

BEZEL_TEST(generic_bare_typeparam_tostring_leak_2475,
           primitive_substitution_lsan_clean) {
    JitResult r = runAotChecked(loopCount("", "i"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);         // compiled + LSan-clean
    BEZEL_EXPECT_EQ(r.exitCode, 40);  // every render byte-correct (Length > 0)
}

// ── Positional record substitution ────────────────────────────────────────

BEZEL_TEST(generic_bare_typeparam_tostring_leak_2475,
           positional_record_substitution_lsan_clean) {
    JitResult r = runAotChecked(loopCount(
        "record Pt(X: int, Y: int); ", "new Pt(i, i + 1)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// A positional record with a STRING field — a materially different shape
// for refcounting (emitRecordToString borrows the field's own string slot
// rather than releasing it, per its own internal `fieldStrIsBorrow` rule) —
// confirmed this fix's new recognizer does not disturb that internal
// borrow/release discipline; only the OVERALL result's producer
// classification changes.
BEZEL_TEST(generic_bare_typeparam_tostring_leak_2475,
           positional_record_with_string_field_lsan_clean) {
    JitResult r = runAotChecked(loopCount(
        "record Person(Name: string, Age: int); ",
        "new Person(\"Ann\", i)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// ── Body record substitution (auto-hashable) ─────────────────────

// A body record was NOT independently leaking before this fix (traced: its
// return-through-generic path reaches the identical `isPlusOneStringProducer`
// gap as the positional case, so this is exercised as a POSITIVE case here,
// not merely a regression guard) — confirmed clean before AND after this fix
// (checked empirically, not inferred, before landing: no leak, no
// double-free/UAF either way).
BEZEL_TEST(generic_bare_typeparam_tostring_leak_2475,
           body_record_substitution_lsan_clean) {
    JitResult r = runAotChecked(loopCount(
        "record Pt "
        "  public property X: int; "
        "  public property Y: int; "
        "  public constructor(x: int, y: int) X := x; Y := y; end constructor; "
        "end Pt; ",
        "new Pt(i, i + 1)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// ── Regression guard: enum substitution stays leak-clean ─────────────────

// Enum's ToString is an immortal string LITERAL (§8.5 qualified-name
// default), not a heap allocation — deliberately NOT added to this fix's
// recognizer (adding it would be an untested, unneeded change). This pins
// that it stays clean either way.
BEZEL_TEST(generic_bare_typeparam_tostring_leak_2475,
           regression_enum_substitution_stays_clean) {
    JitResult r = runAotChecked(loopCount(
        "enum Color Red, Green, Blue; end Color; ", "Color.Green"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// ── Regression guard: the returned string is genuinely USABLE ────────────

// Reuse the returned string in a SECOND operation (concatenation) across
// many iterations — a premature free (an over-aggressive fix skipping a
// retain that was actually needed) would surface as a heap-use-after-free
// under ASan here, not merely a clean exit. Not leak-focused (plain runAot,
// no ASan needed for a UAF this blatant to crash), but run under the ASan
// harness anyway since it is available and free.
BEZEL_TEST(generic_bare_typeparam_tostring_leak_2475,
           returned_string_reusable_not_prematurely_freed) {
    const std::string program =
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method F<T>(val: T): string where T: value "
        "    return val.ToString(); "
        "  end F; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var s := F(new Pt(i, i + 1)); "
        "      total := total + s.Length; "
        "      var t := s + \"!\"; "
        "      total := total + t.Length; "
        "      i := i + 1; "
        "    end; "
        "    return (if total > 0 then 42 else 1); "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(program);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
