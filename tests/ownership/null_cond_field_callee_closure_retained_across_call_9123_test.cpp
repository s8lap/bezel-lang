// Regression test for issue #9123 — a closure INVOKED THROUGH A FUNCTION-TYPED
// FIELD with the NULL-CONDITIONAL spelling `_f?(this)` that displaces the field
// while it runs reads its freed environment (heap-use-after-free).
//
// THE DEFECT. A function value read out of a function-typed holder is a BORROW
// (§28.3, *Function-typed holders*: "A function value **read** from any holder —
// a field, a property or indexer … — carries the empty set"): the read takes no
// ownership, the holder keeps the reference. A class field of function type
// keeps an INDEPENDENT +1 on the closure cell (retained at store by
// `lowerClosureFieldStore`, released on the owner's disposal).
//
// The CALL through that field borrowed the field's reference and took none of
// its own. So when the running closure displaced the field — `_f := null`, or a
// fresh closure — the field's reference was released; if it was the last one,
// the closure cell was freed (with the capture cells §27.5 heap-allocates in
// it) WHILE THE BODY WAS STILL RUNNING, and the body's read of its captured
// variable was a use-after-free.
//
// #9096 fixed the `.` spellings (`_f!(this)`, `F(this)`); the `?.` spelling was
// left out, because it is lowered in a DIFFERENT arm — the null-conditional
// call in `lowerExpr` (LlvmBackend_Exprs.cpp), which lowers the call itself
// rather than dispatching through `lowerCallDispatch`. That arm asked no
// question of the callee at all, so it took no reference of its own.
//
//   MEASURED on the base (r30-c1 @ fec738642), `bezelc --sanitize=address` on
//   the issue's literal repro:
//
//     ERROR: AddressSanitizer: heap-use-after-free on address 0x7b5520de0010
//     READ of size 4 at 0x7b5520de0010 thread T0
//         #0 in lam$0
//         #1 in bz$M.Hx$Run
//     freed by thread T0 here:
//         #1 in bz$lam$0$envdtor
//     SUMMARY: AddressSanitizer: heap-use-after-free bezel in lam$0
//
// THE SPEC.
//   §27.5 (v0.359): "Captured variables are heap-allocated in a closure
//   object" — the captured variable's storage IS the closure object, so the
//   body can only read it while that object exists.
//   §28.9 (v0.359): "Runtime management governs the *function value itself* —
//   the heap-allocated closure object that backs it" — so the object must
//   outlive every call that is still running through it: a function value must
//   not be freed while a call through it is running.
//   §28.3 (v0.359), the *Function-typed holders* bullet: "A function value
//   **read** from any holder — a field, a property or indexer … — carries the
//   empty set" — the read is a borrow, so the holder may release its
//   reference while the call runs, and the CALL is then the only thing that
//   can keep the closure alive.
//   §28.3.11 (v0.359): the discharge of a reference taken for a full
//   expression falls at "the end of the full expression in which it is
//   produced" — here the end of the call expression.
//   §17.12 (v0.359): the null-forgiving `!` is TRANSPARENT — `x!` is `x`'s own
//   read — so `_f!?(this)` reads the field `_f` too.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. Every row asserts
// the program's own output AND that neither sanitizer fired, so a silent
// zero-fill cannot pass. The rows LOOP (200 iterations inside the program), so
// the defect cannot hide under the sanitizer's noise.
//
// `runAot` CANNOT be the oracle here: it does not run the AddressSanitizer
// pass over the generated module (only `runAotChecked` sets
// `opts.sanitizeAddress`), so the use-after-free would go unreported and the
// row would be green on the base — pinning nothing. This is the same reason
// the #9096 rows (tests/ownership/
// field_callee_closure_retained_across_call_9096_test.cpp) use the checked
// oracle.

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

// ROW 1 — the issue's literal repro. The closure is called through the field
// with the NULL-CONDITIONAL spelling (`_f?(this)` — the spelling the issue
// reports), and the body displaces the field (`x.Drop()` sets `_f := null`)
// while it runs.
//
// The sum is `sum(i mod 3 + 1)` over 200 iterations = 399, and every
// iteration's `k + 1` is read out of the closure's environment — the read
// that the base turns into a heap-use-after-free.
//
// RED on the base: a heap-use-after-free in `lam$0` (freed by
// `bz$lam$0$envdtor`).
BEZEL_TEST(null_cond_field_callee_closure_retained_across_call_9123,
           null_conditional_field_callee_displacing_the_field_is_a_uaf) {
    const std::string src =
        "module M; "
        "public class Hx "
        "  private _f: ((Hx) -> int)? := null; "
        "  public constructor() end constructor; "
        "  public method Fill(k: int) _f := lambda(x: Hx) => do x.Drop(); return k + 1; end; end Fill; "
        "  public method Drop() _f := null; end Drop; "
        "  public method Run(): int "
        "    return _f?(this) ?? 7; "
        "  end Run; "
        "end Hx; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var h := new Hx(); h.Fill(i mod 3); t := t + h.Run(); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "399\n");
}

// ROW 2 — the same null-conditional call with the null-forgiving `!` in front
// of the field (`_f!?(this)`). §17.12: `x!` is `x`'s own read — TRANSPARENT —
// so the holder is still the field `_f`, and the call still borrows the
// field's reference. This row pins that the predicate strips the transparent
// wrapper, so the `!` spelling cannot drift from the bare one.
//
// RED on the base: a heap-use-after-free in `lam$0`.
BEZEL_TEST(null_cond_field_callee_closure_retained_across_call_9123,
           null_forgiving_null_conditional_field_callee_displacing_is_a_uaf) {
    const std::string src =
        "module M; "
        "public class Hx "
        "  private _f: ((Hx) -> int)? := null; "
        "  public constructor() end constructor; "
        "  public method Fill(k: int) _f := lambda(x: Hx) => do x.Drop(); return k + 1; end; end Fill; "
        "  public method Drop() _f := null; end Drop; "
        "  public method Run(): int "
        "    return _f!?(this) ?? 7; "
        "  end Run; "
        "end Hx; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var h := new Hx(); h.Fill(i mod 3); t := t + h.Run(); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "399\n");
}

// ROW 3 — the NO-DISPLACEMENT control: the same null-conditional call through
// the same field, with a body that does NOT displace the field. This is the
// shape the fix must leave alone, and it is RED under the plausible wrong fix
// that takes the reference and never gives it back (the "leak instead of
// use-after-free" direction): 200 unreleased references would leave 200 cells
// live at exit and LeakSanitizer would count them.
//
// GREEN on the base and with the fix; it pins that the ordinary
// null-conditional field call still runs, still returns the closure's own
// value, and still leaks nothing.
BEZEL_TEST(null_cond_field_callee_closure_retained_across_call_9123,
           null_conditional_field_callee_without_displacement_is_unaffected) {
    const std::string src =
        "module M; "
        "public class Hx "
        "  private _f: ((Hx) -> int)? := null; "
        "  public constructor() end constructor; "
        "  public method Fill(k: int) _f := lambda(x: Hx) => do return k + 1; end; end Fill; "
        "  public method Run(): int "
        "    return _f?(this) ?? 7; "
        "  end Run; "
        "end Hx; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var h := new Hx(); h.Fill(i mod 3); t := t + h.Run(); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "399\n");
}
