// Regression test for issue #9163 — a closure INVOKED THROUGH AN
// EXPRESSION-BODIED FUNCTION-TYPED PROPERTY whose getter READS A FIELD
// (`property P: (Hx) -> int => _q;`, called as `P(this)`) reads its freed
// environment (heap-use-after-free).
//
// THE DEFECT. A function value read out of a function-typed holder is a BORROW
// (§28.3, *Function-typed holders*: "A function value **read** from any holder —
// a field, a property or indexer … — carries the empty set"): the read takes no
// ownership, the holder keeps the reference. §28.9 (v0.359) says the same of an
// accessor: "A property or indexer **read** is a lend for every declared type …
// a getter never transfers a reclamation obligation to its reader".
//
// An EXPRESSION-bodied getter (§23.2's `property P: T => expr;`, or an accessor's
// `get => expr;`) has NO FRAME OF ITS OWN: its body is INLINED in the reader's
// frame (`computedGetterOf` → `lowerExpr(*getter)`, LlvmBackend_Exprs.cpp). When
// that body READS A FIELD, the value the read yields is the FIELD's own value —
// the field keeps an INDEPENDENT +1 on the closure cell (retained at store by
// `lowerClosureFieldStore`, released on the owner's disposal) — and the reader
// owns NOTHING. The CALL through that property borrowed the field's reference and
// took none of its own. So when the running closure reassigned the field
// (`x.Drop()` sets `_q := lambda(x: Hx) => 0;`), the old cell's reference was
// released; if it was the last one, the closure cell was freed (with the capture
// cells §27.5 heap-allocates in it) WHILE THE BODY WAS STILL RUNNING, and the
// body's read of its captured variable was a use-after-free.
//
// #9096 fixed the FIELD spelling, #9124 the AUTO-PROPERTY spelling and #9147 the
// INTERFACE spelling; the COMPUTED property whose getter reads a field was left
// out, because it answers a DIFFERENT question from the three of them: there the
// callee's symbol IS the holder (a field / auto-property), here the callee's
// symbol is a PropertySymbol and the holder is the field the GETTER reads, one
// level down. `calleeReadsClosureField` is false for it, and #8763's
// `propertyReadMintsClosureRef` is false for it too (the getter's body is a
// BORROW, not a +1 production), so the call took no reference of its own.
//
//   MEASURED on the base (r30-c1 @ 4ece9e7b), `bezelc --sanitize=address` on
//   the issue's literal repro:
//
//     ERROR: AddressSanitizer: heap-use-after-free on address 0x7b1d0a5e0030
//     READ of size 4 at 0x7b1d0a5e0030 thread T0
//         #0 in lam$1
//         #1 in bz$M.Hx$Run
//     freed by thread T0 here:
//         #1 in bz$lam$1$envdtor
//     SUMMARY: AddressSanitizer: heap-use-after-free bezel in lam$1
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
// field_callee_closure_retained_across_call_9096_test.cpp) and the #9124 rows
// (tests/ownership/
// auto_property_callee_closure_retained_across_call_9124_test.cpp) use the
// checked oracle.

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

// ROW 1 — the issue's literal repro. The closure is called through the
// expression-bodied property read directly in the method (`P(this)` — the
// spelling the issue reports), and the body reassigns the FIELD the getter
// reads (`x.Drop()` sets `_q := lambda(x: Hx) => 0;`) while it runs.
//
// The sum is `sum(i mod 3 + 1)` over 200 iterations = 399, and every
// iteration's `k + 1` is read out of the closure's environment — the read
// that the base turns into a heap-use-after-free.
//
// RED on the base: a heap-use-after-free in `lam$1` (freed by
// `bz$lam$1$envdtor`).
BEZEL_TEST(expression_property_callee_closure_retained_across_call_9163,
           expression_property_callee_reassigning_the_field_is_a_uaf) {
    const std::string src =
        "module M; "
        "public class Hx "
        "  private _q: (Hx) -> int := lambda(x: Hx) => 0; "
        "  public property P: (Hx) -> int => _q; "
        "  public constructor() end constructor; "
        "  public method Fill(k: int) _q := lambda(x: Hx) => do x.Drop(); return k + 1; end; end Fill; "
        "  public method Drop() _q := lambda(x: Hx) => 0; end Drop; "
        "  public method Run(): int "
        "    return P(this); "
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

// ROW 2 — the SAME defect through the other EXPRESSION-bodied spelling: the
// getter is an ACCESSOR's expression body (`get => _q;`) inside a block-form
// property. The accessor body is inlined in the reader's frame exactly as the
// property-level body is, so it is the same borrow and the same missing
// reference; this row pins that the two spellings of the expression-bodied
// getter cannot drift apart.
//
// RED on the base: a heap-use-after-free in `lam$1` (freed by
// `bz$lam$1$envdtor`).
BEZEL_TEST(expression_property_callee_closure_retained_across_call_9163,
           expression_accessor_property_callee_reassigning_the_field_is_a_uaf) {
    const std::string src =
        "module M; "
        "public class Hx "
        "  private _q: (Hx) -> int := lambda(x: Hx) => 0; "
        "  public property P: (Hx) -> int get => _q; end; "
        "  public constructor() end constructor; "
        "  public method Fill(k: int) _q := lambda(x: Hx) => do x.Drop(); return k + 1; end; end Fill; "
        "  public method Drop() _q := lambda(x: Hx) => 0; end Drop; "
        "  public method Run(): int "
        "    return P(this); "
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

// ROW 3 — CONTROL: the getter that PRODUCES a FRESH closure
// (`=> lambda(x: Hx) => 7;`). That read MINTS the reader a reference of its
// own (#8981's protocol: the cell is allocated in the reader's frame and the
// reader gives it back), so the call must NOT take a reference of its own on
// top of it — a fix that retained the minted value a second time would leave
// 200 cells live at exit and LeakSanitizer would count them.
//
// GREEN on the base and with the fix; it pins that the fresh-lambda getter is
// not made worse by the fix (the #8981 leak stays fixed).
//
// RED under the plausible wrong fix that retains the getter's minted value a
// second time (or takes a reference for the call and never gives it back).
BEZEL_TEST(expression_property_callee_closure_retained_across_call_9163,
           fresh_lambda_expression_getter_leaks_nothing) {
    const std::string src =
        "module M; "
        "public class Hx "
        "  public property P: (Hx) -> int => lambda(x: Hx) => 7; "
        "  public constructor() end constructor; "
        "  public method Run(): int "
        "    return P(this); "
        "  end Run; "
        "end Hx; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var h := new Hx(); t := t + h.Run(); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1400\n");
}

// ROW 4 — CONTROL: the BLOCK-bodied getter over the same field
// (`get => do return _q; end; end;`). That getter is emitted OUT OF LINE and
// hands the reader a reference of its OWN (#8763's protocol — its `return`
// transfers the returned value's +1, or retains one for a borrowed value), so
// the reader owns one and the call needs none of its own: the reference the
// reader holds is what keeps the closure alive across the invocation.
//
// This is the twin the issue names as already correct, and the fix must leave
// it alone.
//
// GREEN on the base and with the fix.
//
// RED under the plausible wrong fix that extends the new "the getter lends a
// field" classification to the BLOCK-bodied getter AND keeps only the
// reference it took for itself: the getter's own minted reference would then
// never be given back, and 200 cells would leak.
BEZEL_TEST(expression_property_callee_closure_retained_across_call_9163,
           block_property_getter_over_a_field_is_unaffected) {
    const std::string src =
        "module M; "
        "public class Hx "
        "  private _q: (Hx) -> int := lambda(x: Hx) => 0; "
        "  public property P: (Hx) -> int get => do return _q; end; end; "
        "  public constructor() end constructor; "
        "  public method Fill(k: int) _q := lambda(x: Hx) => do x.Drop(); return k + 1; end; end Fill; "
        "  public method Drop() _q := lambda(x: Hx) => 0; end Drop; "
        "  public method Run(): int "
        "    return P(this); "
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
