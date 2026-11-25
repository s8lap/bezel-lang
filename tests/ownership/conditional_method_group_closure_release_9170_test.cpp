// Regression test for issue #9170 — a CONDITIONAL whose arms are BOTH plain
// method groups (`a.Sum((if c then P else Q))`) leaked the 64 B closure cell
// it formed, once per call.
//
// THE DEFECT. §27.6 (v0.359) forms a method reference as a function value, and
// §28.9 (v0.359) puts a function value's backing object under the runtime's
// lifetime: "Function-typed values — values of any function type (T1, ..., Tn)
// -> R, regardless of whether they capture. This includes lambda closures
// (capturing or not), method references (`obj.Method` and `Type.StaticMethod`)"
// — and "Runtime management governs the *function value itself* — the
// heap-allocated closure object that backs it".
//
// A method reference forms a FRESH +1 closure cell at the reference site
// (`makeMethodThunk` -> `buildMethodClosureCell` callocs one rc=1 cell, 64 B;
// the instance spelling adds an 8 B env block). stdlib §5 makes the eager
// operators CALL-BOUNDED: "The **eager** operators (First(pred), …, Sum, Min,
// Max, Average, Aggregate, ToArray, ToDictionary, ToSet) are **call-bounded**
// as their unmarked signatures state — they invoke the callback during the call
// and retain nothing." So the CALL SITE solely owns that +1 and must discharge
// it when the call returns.
//
// The conditional's two arms are such producers, and ONLY the runtime-selected
// arm runs — so the join carries exactly ONE live reference (the #4040
// reasoning: "only the runtime-selected arm's LambdaExpr runs; the unselected
// arm does not allocate a cell, so there is exactly one live reference at the
// join"). The consumer must therefore discharge that one reference, exactly as
// it does for a conditional of two lambda literals.
//
// The producer test that decides this — the SHARED
// `Emitter::isPlusOneClosureProducer` (src/codegen/LlvmBackend_Setup.cpp) —
// gates its ConditionalExpr arm on the conditional's STATIC type being a
// non-nullable `Function`. But sema DEFERS a BARE method reference's Function
// type to the call-resolution slice (`ExprTC::resolveOverload` leaves
// `exprType` unset for a bare reference — the MethodSymbol arm of the same
// predicate says so), and §20's `typeConditional` takes each arm's type: both
// arms' types are unset, so `(if c then P else Q)` has NO expression type AT
// ALL and the gate declined the whole shape. The eager operator's discharge
// never fired and the cell leaked.
//
// MEASURED on the base (r30-c1 @ 026db0dab) with `bezelc --sanitize=address`,
// the issue's own repro (200 iterations):
//
//   Direct leak of 6400 byte(s) in 100 object(s) allocated from:
//       #0 calloc
//   SUMMARY: AddressSanitizer: 12800 byte(s) leaked in 200 allocation(s).
//
// with the program's own output CORRECT (4400). The leak is 64 B per call.
//
// THE FIX. The ConditionalExpr arm of `isPlusOneClosureProducer` now decides
// from the ARMS when the conditional has no static type: both arms must be +1
// producers, and then the join is a closure pointer with exactly one live
// reference, whatever the conditional's static type says. The type-present
// branches of the gate are unchanged (a nullable result is not a uniform +1;
// a non-Function result is not a closure at all), and a conditional that mixes
// a producer with a borrow (a function-typed local, a `null` literal, a
// `throw` arm) still answers false.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. Every row asserts
// the program's own output AND that LeakSanitizer counted nothing, so a
// silent zero-fill cannot pass, and no double free can hide. The rows LOOP
// (200 iterations inside the program), so the leak cannot hide under the
// sanitizer's noise.
//
// `runAot` CANNOT be the oracle here: it does not run the AddressSanitizer
// pass over the generated module (only `runAotChecked` sets
// `opts.sanitizeAddress`), so the leak would go unreported and every row
// would be green on the base — pinning nothing. This is the same reason the
// neighbouring ownership leak row (tests/ownership/
// builtin_selector_method_group_closure_release_9012_test.cpp) uses the
// checked oracle, and lane-contract §5 requires it for disposal/ownership
// work.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when ASan / LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs
                  << " allocation(s) / " << leakedBytes
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_TRUE(sawSummary == false);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor LeakSanitizer fired. `runAotChecked`
// reports an abort and any sanitizer report as `ran == false`.
void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    assertNoLeak(r);
}

}  // namespace

// ROW 1 — the issue's literal repro, amplified 200x. A CONDITIONAL of two
// bare STATIC method groups (`Program.P` / `Program.Q`) handed to the eager
// `Sum` selector. The total is 100 x 11 + 100 x 33 = 4400.
//
// RED on the base: 12800 B leaked in 200 allocations (one 64 B cell per call).
// GREEN with the fix.
BEZEL_TEST(conditional_method_group_closure_release_9170,
           static_method_group_conditional_selector_closure_is_released) {
    const std::string src =
        "module M; "
        "public class Program "
        "  static method P(i: int): int => i; "
        "  static method Q(i: int): int => i * 3; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var a := new Array<int>(); "
        "      a.Add(5); a.Add(6); "
        "      t := t + a.Sum((if i mod 2 = 0 then P else Q)); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "4400\n");
}

// ROW 2 — the SAME conditional handed to a USER method's call-bounded
// function-typed formal (`Use(f: (int) -> int)`). §28.14 (v0.359): "A
// function-typed parameter *without* `scoped` is **call-bounded**: the callee
// may invoke the callback during the call but may not retain it past the
// call" — so the CALL SITE owns the +1 and must discharge it after the call.
// The total is 100 x P(7) + 100 x Q(7) = 100 x 7 + 100 x 21 = 2800.
//
// This is the SECOND consumer of the same predicate (the general call path's
// `producerArg`, src/codegen/LlvmBackend_Calls2.cpp), so it pins that the
// fix reaches the call-bound discharge and not only the eager operator's.
//
// RED on the base: 12800 B leaked in 200 allocations. GREEN with the fix.
BEZEL_TEST(conditional_method_group_closure_release_9170,
           conditional_method_group_argument_to_user_method_is_released) {
    const std::string src =
        "module M; "
        "public class Program "
        "  static method P(i: int): int => i; "
        "  static method Q(i: int): int => i * 3; "
        "  static method Use(f: (int) -> int): int => f(7); "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      t := t + Use((if i mod 2 = 0 then P else Q)); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "2800\n");
}

// ROW 3 — a NESTED conditional of bare method groups. The predicate RECURSES
// into both arms, so the inner conditional must be recognised for the outer
// one to be: `(if i mod 3 = 0 then (if i mod 2 = 0 then P else Q) else
// (if i mod 2 = 0 then Q else P))`.
//
// The total is 101 x 11 + 99 x 33 = 4378 (P is selected for the 101 even-or-
// odd indices the nested selection picks; Q for the other 99).
//
// RED on the base: 12800 B leaked in 200 allocations. GREEN with the fix.
BEZEL_TEST(conditional_method_group_closure_release_9170,
           nested_conditional_method_group_selector_closure_is_released) {
    const std::string src =
        "module M; "
        "public class Program "
        "  static method P(i: int): int => i; "
        "  static method Q(i: int): int => i * 3; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var a := new Array<int>(); "
        "      a.Add(5); a.Add(6); "
        "      t := t + a.Sum((if i mod 3 = 0 then (if i mod 2 = 0 then P else Q) "
        "else (if i mod 2 = 0 then Q else P))); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "4378\n");
}

// ROW 4 — a DIFFERENT eager operator: `Any(pred)`. stdlib §5 declares the
// eager operators as a family, and each one asks the same producer question
// at its own call site, so the fix must reach every eager terminal and not
// only `Sum`. `P(i) = i > 0` and `Q(i) = i > 1` are both true of 5 and 6, so
// the total is 200.
//
// RED on the base: 12800 B leaked in 200 allocations. GREEN with the fix.
BEZEL_TEST(conditional_method_group_closure_release_9170,
           conditional_method_group_selector_on_eager_any_is_released) {
    const std::string src =
        "module M; "
        "public class Program "
        "  static method P(i: int): bool => i > 0; "
        "  static method Q(i: int): bool => i > 1; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var a := new Array<int>(); "
        "      a.Add(5); a.Add(6); "
        "      if a.Any((if i mod 2 = 0 then P else Q)) then t := t + 1; end if; "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "200\n");
}

// CONTROL 1 — the LAMBDA-ARM twin of ROW 1, the shape the issue records as
// already correct ("the twin with one lambda arm is clean after #9012"). It
// must STAY correct: the fix must not turn the conditional's single
// discharge into two.
//
// GREEN on the base and with the fix. RED under a plausible wrong fix — one
// that discharges the callback a second time (e.g. an unconditional release
// beside the producer-gated one, or a release per ARM): the cell would be
// freed twice and AddressSanitizer would report a double free.
BEZEL_TEST(conditional_method_group_closure_release_9170,
           lambda_arm_conditional_twin_stays_clean) {
    const std::string src =
        "module M; "
        "public class Program "
        "  static method P(i: int): int => i; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var a := new Array<int>(); "
        "      a.Add(5); a.Add(6); "
        "      t := t + a.Sum((if i mod 2 = 0 then P else lambda(x: int) => x * 3)); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "4400\n");
}

// CONTROL 2 — the INSTANCE method-group twin of ROW 1 (`s.P` / `s.Q`). §27.6:
// "the instance is captured at formation — by reference for class- or
// interface-typed receivers", so the cell also carries an 8 B env block: the
// cell is 72 B, and a wrong fix that discharged the conditional's env block a
// second time would double-free it.
//
// GREEN on the base and with the fix (a member-access method reference is
// TYPED by sema — only the BARE reference's type is deferred — so the
// conditional carries a Function type and the gate already admitted it).
// RED under a plausible wrong fix — one that releases per ARM, or that
// releases the env block as well as the cell: AddressSanitizer would report
// a double free on the 8 B env block.
BEZEL_TEST(conditional_method_group_closure_release_9170,
           instance_method_group_conditional_twin_stays_clean) {
    const std::string src =
        "module M; "
        "public class Sel "
        "  method P(i: int): int => i; "
        "  method Q(i: int): int => i * 3; "
        "end Sel; "
        "public class Program "
        "  public static method Main(): int "
        "    var s := new Sel(); "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var a := new Array<int>(); "
        "      a.Add(5); a.Add(6); "
        "      t := t + a.Sum((if i mod 2 = 0 then s.P else s.Q)); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "4400\n");
}
