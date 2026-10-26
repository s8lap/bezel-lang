// Codegen — §12.7 / §8.6 / §28.9, #3116 (post-#3200, v0.254): a §12.7
// value-narrowed READ of a boxed `Object`/interface local, when the read is
// the operand of an ORDINARY BINARY OPERATOR (`o + 1`) or an ORDINARY CALL
// ARGUMENT (`Take(o)`), is a NON-CONSUMING READ of the box's payload — the
// same shape the existing `is`/`as` / null-comparison / direct method call
// / console-argument arms already admit as safe reads.
//
// spec §12.7 (v0.252), *Type tests: `is`, `is not`, `as`*:
//   "`is T` tests whether a value is an instance of `T` and produces `bool`.
//    When the test succeeds and the tested expression is a simple variable, the
//    variable is **flow-narrowed** to `T` within the controlled block"
//   "Both `is` and `as` are **non-destructive on their operand**: the operation
//    inspects the runtime type tag without moving from the operand."
//
// spec §8.6 (v0.252), *Boxing*:
//   "When a value type is used where `Object` (or an interface implemented by
//    the value type) is expected, the value is **boxed**: a heap-allocated
//    wrapper holds the value."
//   "leaves it by being read out — a narrowed read ([§12.7](#sec-type-tests-is-is-not-as)),
//    an unboxing `as` ([§17.4](#sec-the-is-is-not-and-as-operators)), a typed-
//    pattern binding over a boxed scrutinee ([§19.3](#sec-patterns)), or the
//    call-bounded receiver copy a call on the box takes ([§8.5](#sec-tostring))"
//
// spec §28.9 (v0.252), *Runtime-managed types*:
//   "Boxed value types — created implicitly when a value type is used where
//    `Object` (or an interface) is expected; a box's payload is fixed at
//    boxing and no operation this revision defines writes it ([§8.6](#sec-boxing),
//    [§12.7](#sec-type-tests-is-is-not-as) — the shapes that could appear to
//    are compile errors there), so a boxed value is an immutable shared
//    reference."
//
// THE DEFECT — #3116 measured at the #3090 fix commit:
//
//     var i := 0;
//     while i < 20 do
//       var o: Object := 42;
//       if o is int then
//         Console.WriteLine(o);        // (A) — LSan-clean (console arg, #2865)
//         Console.WriteLine(o + 1);    // (B) — 480 B / 20 iters leaked (24 B / box)
//       end;
//       i := i + 1;
//     end;
//
// `BoxEscapeWalker` (src/codegen/LlvmBackend_Setup.cpp) has a small positive
// list of non-retaining read positions: a type-test operand, a `= null` /
// `<> null` comparison, a direct method call on the local, and a console
// argument. Everything else is an escape, which suppresses the §28.9 scope-
// end `bz_box_release`. The three expression shapes that the narrowing lowers
// to a payload copy but the walker classifies as escape:
//
//   * a LocalDecl initializer (`var i: int := o;`) — fixed by #3200
//   * any other binary operator (`o + 1`) — fixed by #3116 (this fix)
//   * an ordinary call argument (`Take(o)`) — fixed by #3116 (this fix)
//
// Post-#3090 all three lower to a payload copy — `narrowedBoxPayloadAddr` /
// `unboxNarrowedRead` read the box's PAYLOAD BYTES, never the wrapper, so
// the box stays uniquely held by `o`. On the read side they are as non-
// retaining as the `is`/`as` operand the walker already admits; on the
// release side the wrapper's +1 is still owned by `o` and the scope-end
// `bz_box_release` is correct.
//
// THE FIX widens `BoxEscapeWalker::expr`:
//   * the BinaryExpr arm — admit a §12.7-narrowed identifier operand
//     (the `=` / `<>` null-literal arms stay as-is; a non-null RHS keeps the
//     defensive escape treatment the issue cites).
//   * `walkArgs` — admit a §12.7-narrowed identifier argument, alongside the
//     existing console-argument carve (#2865).
//
// Both widenings share one predicate (`BoxEscapeWalker::isNarrowedRead`),
// which mirrors the `narrowedIdentDeclaredType` half `narrowedBoxPayloadAddr`
// already uses (#3090) and the LocalDecl arm `narrowedRead` already uses
// (#3200): a BARE IDENTIFIER, declared type Object/interface, current
// resolved type non-Object/interface value type. Same answer everywhere —
// inside the narrowed block the resolved type is the value type (the read is
// safe), outside it the resolved type is still Object/interface (the read
// stays an escape). Inside a lambda the safe-read is suppressed: a captured
// local lives in the env, which outlives the producing scope (FINDING [1]
// in the walker, mirrors the #2960 lambda-capture guard for strings).

#include <string>

#include "support/aot_result_asserts.hpp"   // fix #3895: diagnosticMessageContains
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::diagnosticMessageContains;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

}  // namespace

// =====================================================================
// THE DEFECT'S OWN SHAPE — binary operator `o + 1` in a 10k-iteration
// loop. The issue measured 480 B / 20 iters at #3090's commit (24 B per
// `int` box). With this fix the box release runs at the loop body's
// scope end and LSan is clean. The exit code is the LAST accumulator
// value (when `i = 9999`) so the loop can run large enough to make the
// per-iteration leak LSan-detectable, mirroring the #3200 control tests'
// `return 42 when b = 9999` shape.
// =====================================================================
BEZEL_TEST(codegen_narrowed_value_op_arg_release_3116,
           narrowed_int_binary_op_loop_does_not_leak_box_spec_12_7_28_9) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var i := 0; var acc := 0; "
        "    while i < 10000 do "
        "      var o: Object := i; "
        "      if o is int then "
        "        acc := acc + (o + 1); "
        "        if acc > 5000000 then return 42; end; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    return 7; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    // Pre-fix the box leaked (24 B × 10000 = 240 KB, LSan-flagged); with
    // this fix the release runs, `acc` reaches the sentinel, Main returns
    // 42. r.ran is the load-bearing assertion (LSan-clean + box release
    // armed); exit code 42 pins that `o` was unboxed to its value, not the
    // box pointer (a non-overflow acc would mean the binary read returned
    // garbage).
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// =====================================================================
// THE SECOND DEFECT SHAPE — ordinary call argument `Take(o)` in a 20-
// iteration loop. Same leak signature (24 B × iter), same fix mechanism.
// The callee takes an `int` parameter — a copy of the payload bytes,
// never the wrapper — so the box stays uniquely held by `o` and the
// scope-end release is correct. The exit code is the LAST accumulator
// value (when `i = 9999`) so the loop can run large enough to make the
// per-iteration leak LSan-detectable, mirroring the #3200 control tests'
// `return 42 when b = 9999` shape.
// =====================================================================
BEZEL_TEST(codegen_narrowed_value_op_arg_release_3116,
           narrowed_int_call_argument_loop_does_not_leak_box_spec_12_7_28_9) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Take(n: int): int => n + 100; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 10000 do "
        "      var o: Object := i; "
        "      if o is int then "
        "        var b := Take(o); "
        "        if b = 10099 then return 42; end; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    return 7; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    // b == 10099 only when i == 9999 (Take(9999) = 9999 + 100 = 10099).
    // Pre-fix the box leaked (24 B × 10000 = 240 KB, LSan-flagged); with
    // this fix the release runs, Main reaches the `b == 10099` arm and
    // returns 42.
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// =====================================================================
// THE ISSUE'S OWN EXACT SHAPE — Console.WriteLine(o) followed by
// Console.WriteLine(o + 1) in a 20-iteration loop. #2865 (console arg)
// already arms the release for (A); the bug surfaces when (B) is added.
// Pins that the new binary-op widening does not regress the console
// carve and that both reads of the same box in the same iteration stay
// LSan-clean. 20 iters × (one "42" from (A) and one "43" from (B)) =
// 40 lines; the value oracle is the line count, not the sum.
// =====================================================================
BEZEL_TEST(codegen_narrowed_value_op_arg_release_3116,
           narrowed_int_console_arg_then_binary_op_no_leak_spec_12_7_28_9) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 20 do "
        "      var o: Object := 42; "
        "      if o is int then "
        "        Console.Write(o); "
        "        Console.WriteLine(o + 1); "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::size_t n43 = 0, at = 0;
    while ((at = r.stdoutText.find("43\n", at)) != std::string::npos) {
        ++n43; at += 3;
    }
    BEZEL_EXPECT_EQ(n43, static_cast<std::size_t>(20));
}

// =====================================================================
// CONTROL — a non-narrowed binary operator on an `Object` is now a CLEAN
// SEMA REJECTION, not a leak. Outside a §12.7 narrowed block the resolved
// type of `o` stays `Object`, so this row never reaches the box-escape/
// release path this file otherwise exercises — there is no box to leak.
//
// History: this row's original (#3116-era) comment claimed the program
// leaks (480 B / 20 iters, LSan-flagged) and was pinned with a bare `!ran`
// claim. Fix #3895 (issue #3892) found that claim false — the program does
// NOT reach the box-escape/leak path at all: sema admitted `o + 1` for
// `o: Object` with no matching `operator +`, and codegen emitted
// `%_ = add ptr %o, i32 1` (an int literal added to a pointer), which
// failed LLVM's IR verifier before the program ever ran. #3895 pinned that
// ACTUAL (buggy) behavior as issue 3903, rather than reverting to the
// disproven leak claim.
//
// fix #3903 (§3.1/§3.4/§3.5/§8.1/§25/§26.3, v0.294): sema's binary-operator
// typecheck (`ExprTypecheck_Exprs.cpp`'s `typeBinary`) now rejects an
// `Object`-typed operand of `+`/`-`/`*`/`/`/`mod`/`&`/`|`/`^`/`<<`/`>>`/`<`/
// `>`/`<=`/`>=` with a clean diagnostic — the same "no applicable overload"
// verdict `operator_operand_order_1096_test.cpp`'s family gets for an
// undeclared record operator — instead of reaching codegen typed as an
// ordinary primitive. This row is re-pinned a second time to assert the
// diagnostic TEXT of the fix rather than the codegen crash it replaces; see
// `binary_op_object_operand_rejected_3903_test.cpp` for the full grid.
BEZEL_TEST(codegen_narrowed_value_op_arg_release_3116,
           unnarrowed_object_binary_op_rejected_by_sema_3903) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 20 do "
        "      var o: Object := i; "
        "      var dummy := o + 1; "
        "      i := i + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    // fix #3903: a clean sema rejection at a real source location, naming
    // the operator and `Object` — never `hasErrors()`/`!ran` alone, which
    // could not tell this apart from the IR-verification crash it replaces.
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `+` is not defined for type `Object`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""25"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
}

// =====================================================================
// CONTROL — a non-narrowed call argument MUST still leak the box. Same
// gating story as the binary-op control above: outside the narrowed block
// `isNarrowedRead` declines, `walkArgs` keeps its conservative escape
// verdict, and LSan flags the leak.
// =====================================================================
BEZEL_TEST(codegen_narrowed_value_op_arg_release_3116,
           unnarrowed_object_call_argument_still_leaks_as_expected) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Take(n: Object): int => 0; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 20 do "
        "      var o: Object := i; "
        "      var dummy := Take(o); "
        "      i := i + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    // fix #3895 (issue #3892): pin that the failure is SPECIFICALLY the
    // expected leak report, not a compiler crash / unrelated runtime signal
    // that a bare `!ran` cannot tell apart from the leak this row wants.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "LSan/ASan at exit"));
}
