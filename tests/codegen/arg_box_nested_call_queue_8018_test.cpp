// Codegen — §8.6 / §28.9 / §28.3 / #8018: a §8.6 ARGUMENT BOX that is MINTED
// but never QUEUED when a NESTED CALL appears inside the argument expression.
//
// THE DEFECT (#8018). `buildOrderedArgs` mints the §8.6 box for an argument
// and then asks `isCalleeParamNonEscaping(currentCallee_, idx)` whether the
// callee keeps that parameter (§8.6 / §28.9 / §28.3). `currentCallee_` is
// EMITTER STATE, not a per-call value: the call site installs the callee on
// it before `buildOrderedArgs` runs, but a NESTED CALL inside an argument
// expression CLOBBERS it — the nested call installs its own callee and clears
// `currentCallee_` at its own end (`emitMethodCall` / `lowerCallDispatch`).
//
// So by the time the OUTER call's mint check runs, `currentCallee_` is null:
// the check sees "no callee", the freshly minted box is NEVER put on the
// pending-release queue, and nothing else ever releases it — **24 bytes
// leaked per call**, unbounded in a loop (the #2952 family's shape).
//
// The same clobbering can also leave the WRONG callee behind: a nested
// GENERIC call returns from `lowerCallDispatch` WITHOUT clearing
// `currentCallee_` (its arm returns before the tail that clears it), so the
// outer call's check asks about the NESTED callee — a box could be released
// although THIS call's callee keeps the parameter.
//
// THE FIX. The callee of the call BEING BUILT is captured ONCE, at the top of
// `buildOrderedArgs` (before any argument expression is lowered), and the
// mint check consults the capture. The capture is trusted only when it
// provably IS this call's callee (the formal list `params` is the callee's own
// vector), so a stale `currentCallee_` left by a call site that installs no
// callee of its own is rejected.
//
// §8.6 (v0.355): "When a value type is used where `Object` (or an interface
//   implemented by the value type) is expected, the value is **boxed**: a
//   heap-allocated wrapper holds the value. Boxed values are runtime-managed
//   ([§28.9])."
// §28.9 (v0.355): "Boxed value types — created implicitly when a value type
//   is used where `Object` (or an interface) is expected" are "exempt from
//   manual disposal" — which exempts the USER from writing a disposal, NOT
//   the compiler from performing one.
// §28.3 (v0.355): an unbound owned temporary "is disposed at the end of the
//   full expression in which it is produced, after every use of it within
//   that expression has completed" — for an argument-position §8.6 box, that
//   is "right after the call returns".
//
// ⚠ ORACLE DISCIPLINE.
//  * The rows loop 100× so the 24-byte-per-call leak is decisive under LSan
//    (CLAUDE.md, "Vacuous-oracle traps": a leak repro that runs once can hide
//    under the sanitizer's noise).
//  * Every row asserts a NUMBER read back out of the box (§8.6's payload),
//    never a constant, so a row cannot pass by accident.
//  * The rows use `runAotChecked` (generated-code ASan + LSan). A leak makes
//    the row FAIL: with `BEZEL_AOT_DETECT_LEAKS` unset the leak report is
//    matched by the harness's sanitizer check (`hasSanitizerReport`), which
//    reports an Error and clears `ran`.
//  * The escape row (G) uses `runAotCheckedLenient` (LSan OFF, ASan ON) — the
//    same choice tests/codegen/arg_position_box_release_2952_test.cpp makes
//    for its over-release rows: there the LEAK is the EXPECTED outcome (a
//    callee that keeps the parameter is not the caller's to release), so the
//    row's job is to catch a PREMATURE release (a use-after-free), which ASan
//    reports and LSan would only mask.
//
// ⚠ WHY THE FORMALS ARE SPELLED `System.Object` (§8.1, v0.355).
// "a nested type or a type parameter named `Object` shadows the root under
// §2.7 as any declaration shadows an always-available name, and
// `System.Object` still reaches it". The issue's own program spells the
// formal `x: Object` INSIDE a generic method that declares a type parameter
// named `Object` — so that `Object` is the TYPE PARAMETER, not the root, the
// formal's type substitutes to the argument's own value type, and NO §8.6 box
// is minted at all (MEASURED: that program leaks nothing). The §8.6 box is
// minted only for the ROOT `Object`, spelled `System.Object`. The rows below
// therefore use the qualified spelling, exactly as
// tests/codegen/arg_box_release_call_keyed_7998_test.cpp documents for its
// own rows.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// ── The programs ────────────────────────────────────────────────────────────
//
// Each program loops 100× so the per-call box is decisive under LSan, and
// each prints a value READ OUT of the box's payload (§8.6) so the row's
// expectation is a number, not a constant.

// (A) THE ISSUE'S OWN REPRO, in the shape that actually MINTS a box: the
// formal is the root `Object` (`System.Object`), the argument is a CALL
// (`Inner()`), and the call is generic (`Outer<int>(...)`) exactly as the
// issue spells it. The nested call's `emitMethodCall` clears `currentCallee_`
// before the outer call's own mint check runs.
// 100 x 7 = 700.
const char* kIssueRepro =
    "module M8018A; "
    "public class Program "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 100 do "
    "      t := t + Outer<int>(Inner()); "
    "      k := k + 1; "
    "    end while; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "  public static method Outer<Object>(x: System.Object): int "
    "    => (x as int) ?? 0; "
    "  public static method Inner(): int => 7; "
    "end Program;";

// (B) A nested call as the WHOLE argument (the non-generic twin of (A)).
// 100 x 7 = 700.
const char* kWholeArgument =
    "module M8018B; "
    "public class Program "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 100 do "
    "      t := t + Outer(Inner()); "
    "      k := k + 1; "
    "    end while; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "  public static method Outer(x: System.Object): int "
    "    => (x as int) ?? 0; "
    "  public static method Inner(): int => 7; "
    "end Program;";

// (C) A nested call inside a BINARY EXPRESSION: the argument is `1 + Inner()`.
// The expression is evaluated first (the nested call clears `currentCallee_`),
// and only THEN is the int result boxed for the `System.Object` formal.
// 100 x 8 = 800.
const char* kBinaryExpression =
    "module M8018C; "
    "public class Program "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 100 do "
    "      t := t + Outer(1 + Inner()); "
    "      k := k + 1; "
    "    end while; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "  public static method Outer(x: System.Object): int "
    "    => (x as int) ?? 0; "
    "  public static method Inner(): int => 7; "
    "end Program;";

// (D) A nested call inside an INTERPOLATION: the argument expression is
// `$"{(Inner())}".Length` — an int (so the formal still gets a §8.6 box),
// computed through an interpolation whose hole holds the nested call.
// "7".Length = 1, so 100 x 1 = 100.
const char* kInsideInterpolation =
    "module M8018D; "
    "public class Program "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 100 do "
    "      t := t + Outer($\"{(Inner())}\".Length); "
    "      k := k + 1; "
    "    end while; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "  public static method Outer(x: System.Object): int "
    "    => (x as int) ?? 0; "
    "  public static method Inner(): int => 7; "
    "end Program;";

// (E) TWO LEVELS DEEP: `Outer(Inner2(Inner()))`. Two boxes are minted per
// iteration — one for `Inner2`'s formal and one for `Outer`'s — and TWO
// nested calls clobber the emitter state (the inner one inside `Inner2`'s own
// argument list, the outer one before `Outer`'s box is minted).
// 100 x 7 = 700.
const char* kTwoLevelsDeep =
    "module M8018E; "
    "public class Program "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 100 do "
    "      t := t + Outer(Inner2(Inner())); "
    "      k := k + 1; "
    "    end while; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "  public static method Outer(x: System.Object): int "
    "    => (x as int) ?? 0; "
    "  public static method Inner2(v: System.Object): int "
    "    => (v as int) ?? 0; "
    "  public static method Inner(): int => 7; "
    "end Program;";

// (F) A nested call as the RECEIVER of a method call: the argument expression
// is `Inner().ToString().Length` — the nested call is the receiver of the
// member call (§9.4), and the whole expression yields an int, so the formal
// still gets a §8.6 box. "7".Length = 1, so 100 x 1 = 100.
const char* kReceiverOfMethodCall =
    "module M8018F; "
    "public class Program "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 100 do "
    "      t := t + Outer(Inner().ToString().Length); "
    "      k := k + 1; "
    "    end while; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "  public static method Outer(x: System.Object): int "
    "    => (x as int) ?? 0; "
    "  public static method Inner(): int => 7; "
    "end Program;";

// (G) THE ESCAPE ROW — the callee KEEPS the parameter, so the box must NOT be
// released early. `Sink.Outer` stores its parameter into an instance field
// (`_saved := x`), which the escape analysis reads as an ESCAPE (§28.3): the
// box is the CALLEE's now, and the caller must not `bz_box_release` it. The
// nested call is a GENERIC one (`Generic<int>(Inner())`), whose arm returns
// from `lowerCallDispatch` WITHOUT clearing `currentCallee_` — so on the base
// the outer call's check consulted the NESTED callee (which does not escape)
// instead of its own.
//
// The row's job is the OVER-RELEASE direction: if the fix released the box,
// the later `sink.Read()` would read a freed box and ASan would report
// "heap-use-after-free". The LEAK is the EXPECTED outcome here (a callee that
// keeps the parameter is not the caller's to release — the same reasoning as
// tests/codegen/arg_position_box_release_2952_test.cpp's over-release rows),
// so this row runs with LSan OFF and ASan ON.
//
// 100 x 1 (the stored value) + 7 (the value read back out of the box) = 107.
const char* kEscapingCallee =
    "module M8018G; "
    "public class Sink8018G "
    "  public _saved: System.Object; "
    "  public constructor() _saved := null; end constructor; "
    "  public method Outer(x: System.Object): int "
    "    _saved := x; "                       // ESCAPE - instance-field store
    "    return 1; "
    "  end Outer; "
    "  public method Read(): int "
    "    return (_saved as int) ?? 0; "
    "  end Read; "
    "end Sink8018G; "
    "public class Program "
    "  public static method Main(): int "
    "    using var sink := new Sink8018G(); "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 100 do "
    "      t := t + sink.Outer(Generic<int>(Inner())); "
    "      k := k + 1; "
    "    end while; "
    "    Console.WriteLine(t + sink.Read()); "
    "    return 0; "
    "  end Main; "
    "  public static method Generic<T>(y: System.Object): int "
    "    => (y as int) ?? 0; "
    "  public static method Inner(): int => 7; "
    "end Program;";

// (H) THE CONTROL — no nesting at all. The argument is a plain local, so
// `currentCallee_` still holds the callee at the mint check and the box was
// already queued and released before this fix. The row pins that the fix does
// not change the control's behaviour. sum(k, k=0..99) = 4950.
const char* kControlNoNesting =
    "module M8018H; "
    "public class Program "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 100 do "
    "      t := t + Outer(k); "
    "      k := k + 1; "
    "    end while; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "  public static method Outer(x: System.Object): int "
    "    => (x as int) ?? 0; "
    "end Program;";

}  // namespace

// =====================================================================
// (A) THE ISSUE'S OWN REPRO — the §8.6 box minted in the CALLER must be
// released by the CALLER, after the call returns.
//
// WITHOUT THE FIX: the nested `Inner()` clears `currentCallee_`, so the box
// is minted and never queued. MEASURED on the base: 100 x 24 B = 2400 B
// leaked in 100 objects; the row fails (the harness's sanitizer check reports
// the leak and clears `ran`).
// =====================================================================

// per §8.6 / §28.9 / §28.3, v0.355
BEZEL_TEST(codegen_arg_box_nested_call_queue_8018,
           the_issues_own_repro_queues_the_box_spec_8_6) {
    JitResult plain = runAot(kIssueRepro);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.stdoutText, "700\n");
    JitResult checked = runAotChecked(kIssueRepro);
    BEZEL_EXPECT_TRUE(checked.ran);   // pre-fix: 2400 B leaked in 100 objects
    BEZEL_EXPECT_EQ(checked.stdoutText, "700\n");
}

// =====================================================================
// (B) A nested call as the WHOLE argument — the non-generic twin of (A).
// =====================================================================

// per §8.6 / §28.9 / §28.3, v0.355
BEZEL_TEST(codegen_arg_box_nested_call_queue_8018,
           nested_call_as_the_whole_argument_queues_the_box_spec_8_6) {
    JitResult plain = runAot(kWholeArgument);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.stdoutText, "700\n");
    JitResult checked = runAotChecked(kWholeArgument);
    BEZEL_EXPECT_TRUE(checked.ran);   // pre-fix: 2400 B leaked in 100 objects
    BEZEL_EXPECT_EQ(checked.stdoutText, "700\n");
}

// =====================================================================
// (C) A nested call inside a BINARY EXPRESSION.
// =====================================================================

// per §8.6 / §28.9 / §28.3, v0.355
BEZEL_TEST(codegen_arg_box_nested_call_queue_8018,
           nested_call_inside_a_binary_expression_queues_the_box_spec_8_6) {
    JitResult plain = runAot(kBinaryExpression);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.stdoutText, "800\n");
    JitResult checked = runAotChecked(kBinaryExpression);
    BEZEL_EXPECT_TRUE(checked.ran);   // pre-fix: 2400 B leaked in 100 objects
    BEZEL_EXPECT_EQ(checked.stdoutText, "800\n");
}

// =====================================================================
// (D) A nested call inside an INTERPOLATION.
// =====================================================================

// per §8.6 / §28.9 / §28.3, v0.355
BEZEL_TEST(codegen_arg_box_nested_call_queue_8018,
           nested_call_inside_an_interpolation_queues_the_box_spec_8_6) {
    JitResult plain = runAot(kInsideInterpolation);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.stdoutText, "100\n");
    JitResult checked = runAotChecked(kInsideInterpolation);
    BEZEL_EXPECT_TRUE(checked.ran);   // pre-fix: 2400 B leaked in 100 objects
    BEZEL_EXPECT_EQ(checked.stdoutText, "100\n");
}

// =====================================================================
// (E) TWO LEVELS DEEP — two boxes and two clobbering nested calls.
// =====================================================================

// per §8.6 / §28.9 / §28.3, v0.355
BEZEL_TEST(codegen_arg_box_nested_call_queue_8018,
           two_levels_deep_queue_both_boxes_spec_8_6) {
    JitResult plain = runAot(kTwoLevelsDeep);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.stdoutText, "700\n");
    JitResult checked = runAotChecked(kTwoLevelsDeep);
    BEZEL_EXPECT_TRUE(checked.ran);   // pre-fix: leaked boxes
    BEZEL_EXPECT_EQ(checked.stdoutText, "700\n");
}

// =====================================================================
// (F) A nested call as the RECEIVER of a method call.
// =====================================================================

// per §8.6 / §28.9 / §28.3, v0.355
BEZEL_TEST(codegen_arg_box_nested_call_queue_8018,
           nested_call_as_the_receiver_queues_the_box_spec_8_6) {
    JitResult plain = runAot(kReceiverOfMethodCall);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.stdoutText, "100\n");
    JitResult checked = runAotChecked(kReceiverOfMethodCall);
    BEZEL_EXPECT_TRUE(checked.ran);   // pre-fix: 2400 B leaked in 100 objects
    BEZEL_EXPECT_EQ(checked.stdoutText, "100\n");
}

// =====================================================================
// (G) THE OVER-RELEASE GUARD — a callee that KEEPS the parameter must not
// have its box released by the caller. The read-back of the stored box must
// not be a use-after-free.
//
// LSan is OFF for this row (the leak is the EXPECTED outcome of an escaping
// parameter — the same choice the #2952 over-release rows make); ASan is ON,
// so a premature `bz_box_release` surfaces as "heap-use-after-free" and the
// row fails.
// =====================================================================

// per §28.3 / §28.9 — a parameter the callee stores is the callee's.
BEZEL_TEST(codegen_arg_box_nested_call_queue_8018,
           escaping_callee_box_is_not_released_early_spec_28_3) {
    JitResult checked = runAotCheckedLenient(kEscapingCallee);
    BEZEL_EXPECT_TRUE(checked.ran);   // no heap-use-after-free
    BEZEL_EXPECT_EQ(checked.stdoutText, "107\n");
}

// =====================================================================
// (H) THE CONTROL — no nesting: the box was already queued and released
// before this fix, and the fix must not change that.
// =====================================================================

// per §8.6 / §28.9 / §28.3, v0.355
BEZEL_TEST(codegen_arg_box_nested_call_queue_8018,
           control_without_nesting_still_releases_the_box_spec_8_6) {
    JitResult plain = runAot(kControlNoNesting);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.stdoutText, "4950\n");
    JitResult checked = runAotChecked(kControlNoNesting);
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.stdoutText, "4950\n");
}
