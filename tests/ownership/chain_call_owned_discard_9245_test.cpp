// Ownership / sema — issue #9245: the bare statement `C.Make()();` — calling
// a function value RETURNED BY A CALL, whose result has a non-trivial dispose
// — drew only the §21.9 unused-result WARNING (rc 0), while its neighbours
// `var f := C.Make(); f();` and `C.Mk();` are §21.9 hard errors.
//
// THE RULE. §21.9 (v0.359) *Statement form of expressions*, the escalation
// sentence, verbatim:
//
//   "The escalation is one rule over three kinds — it is not `Task`-specific —
//    and it is keyed on the result's **static type**, not on its §28.3
//    ownership classification: a bare-statement call whose result type is a
//    non-trivial-dispose class or an interface is the hard error whether the
//    result is an owned production or a borrow, the fluent-receiver discard
//    above being the sole exemption"
//
// and the remedy the hard error names is §17.13's discard form `_ := expr;`.
//
// The result of `C.Make()()` is an OWNED production of the class `W` — the
// function type `() -> owned W` says the call produces an owned value, and
// `W` carries a destructor, so it is non-trivial-dispose (§28.9). The section
// therefore decides the case: it must be the hard error, with the SAME
// diagnostic the neighbours draw.
//
// MEASURED on the base (r30-c1 @ 2b3839abd, `bezelc -c`):
//
//   | program                              | base rc | base diagnostic                    |
//   |--------------------------------------|---------|------------------------------------|
//   | `C.Make()();`  (W has a destructor)  |   0     | §21.9 unused-result WARNING only   |
//   | `var f := C.Make(); f();`            |   1     | "this expression produces an owned  |
//   |                                     |         |  value that requires disposal …"    |
//   | `C.Mk();`                           |   1     | the same hard error                 |
//   | `_ := C.Make()();`                   |   0     | none                               |
//
// ROOT CAUSE (sema, §21.9's own walk in src/sema/ExprTypecheck.cpp). The
// walk's `ownedProduced` predicate asks "does this statement discard an OWNED
// production?", and for a call whose CALLEE IS A FUNCTION VALUE it reads the
// `owned`-return marker off the callee SYMBOL — a LocalSymbol /
// ParameterSymbol / FieldSymbol whose Function-typed `declType` carries
// `funcReturnOwned` (the #2680 arm (d)). The CHAINED-CALL callee
// (`C.Make()`) is a CallExpr: it resolves to no such symbol, and the
// MethodSymbol path (`calleeReturnsOwned`) reads the INNER call's own return —
// a function VALUE, which is not an owned production. The `owned`-return fact
// was therefore unavailable and the statement fell through to the
// unused-result warning.
//
// THE FACT, READ WHERE IT LIVES. The marker of a function VALUE is on its
// Function TypeRef's `funcReturnOwned` — the same place the FieldSymbol arm
// reads it — and the callee's STATIC TYPE carries it here, because the callee
// is an EXPRESSION (`C.Make()`) whose type IS the function type `() -> owned
// W` this call is about to invoke. The fix reads it there, and only for a
// callee that is ITSELF a call, so no other callee shape's verdict changes.
//
// §28.3.11 (v0.359) — the second half of the fix, and the reason it is NOT a
// one-liner. An unbound owned temporary "is disposed at the end of the full
// expression in which it is produced", and for an expression statement
// "end of the full expression" and "end of the statement" coincide (§21.9).
// For THIS shape that disposal is ALREADY emitted by codegen: the chain-call
// lowering mints the `ccres.slot` and registers the scope-end disposal for an
// UNBOUND chain-call result (src/codegen/LlvmBackend_Calls2.cpp — the #9082 /
// #9138 / #9141 work). So §21.9's OWN reclaim flag (`disposeOwnedValue`)
// must NOT be set for the chain-call shape: it would be a SECOND free of one
// value.
//
// MEASURED (ASan, `bezelc --sanitize=address`, a 200-iteration loop over a
// trivial-dispose `W`): with the reclaim flag set for the chain-call shape the
// program aborts with
//
//   ERROR: AddressSanitizer: heap-use-after-free … READ of size 8 …
//     freed by thread T0 here: free … in main (the chain-call result's
//     disposal)
//
// — §21.9's free ran first and codegen's own chain-call disposal then read
// the freed object's vtable. The BOUND twin (`var f := C.Make(); f();`) has
// no chain-call disposal, so its §21.9 flag is the only free and it stays
// correct. `chained_call_trivial_dispose_loop_reclaimed_spec_28_3_11` below
// is the row that pins that: it goes RED (an ASan heap-use-after-free) under
// that wrong fix.
//
// #6572 (§21.9, v0.338): the escalation REPLACES the unused-result warning —
// one §21.9 message per regime, not two. `chained_call_bare_statement_…`
// below asserts the warning is ABSENT, so the fix cannot stack the error and
// the warning at the same file:line:col.
//
// The rows below are RED on the base and GREEN with the fix, except the two
// named controls, which are green on BOTH arms by design (lane contract §7):
// the `_ :=` twin is §21.9's own remedy and must stay admitted, and the
// trivial-dispose loop is the double-free guard. The base-green twins the
// issue lists as already correct (`var f := C.Make(); f();`, `C.Mk();`) are
// NOT gtest rows — they are green on the base and pin nothing; they are in
// the probe matrix and in `corpus-candidates/`.
//
// per §21.9, §17.13, §28.3, §28.3.11, §28.9, §27.7 (issue #9245, v0.359)

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageCount;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The §21.9 hard error's own wording. Matched on the RULE's words plus the
// §-citation, so a different §21.9 diagnostic (the `Task` arm's wording, the
// unused-result warning) cannot satisfy it.
const char kOwnedDiscard[] =
    "this expression produces an owned value that requires disposal";

// The §21.9 unused-result WARNING's wording — #6572: the escalation
// REPLACES it, so a program that draws the hard error must not draw it too.
const char kUnusedResult[] =
    "the result of this expression statement is unused";

// Every diagnostic the harness produced, concatenated.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// The issue's program: a class `W` with a destructor (so it is
// non-trivial-dispose, §28.9) and a static factory `Make(): () -> owned W`
// whose function value produces an owned `W`. `body` is the method body of
// `Main`.
std::string prog(const std::string& body) {
    return "module M; "
           "public class W "
           "  public V: int := 3; "
           "  destructor "
           "  end destructor; "
           "end W; "
           "public class C "
           "  public static method Make(): () -> owned W "
           "    return lambda(): W => new W(); "
           "  end Make; "
           "  public static method Main(): int " +
           body +
           "  end Main; "
           "end C;";
}

// The TRIVIAL-dispose twin of the same program: `W` has no destructor, so it
// is §28.9-trivial-dispose and the statement is NOT a §21.9 hard error — it
// is the shape that exercises the RECLAIM side of the fix.
std::string trivialProg(const std::string& body) {
    return "module M; "
           "public class W "
           "  public V: int := 3; "
           "end W; "
           "public class C "
           "  public static method Make(): () -> owned W "
           "    return lambda(): W => new W(); "
           "  end Make; "
           "  public static method Main(): int " +
           body +
           "  end Main; "
           "end C;";
}

// True iff SOME diagnostic carries §21.9's non-trivial-dispose hard error.
bool rejectedBareOwnedProducer(const JitResult& r) {
    const std::string text = diagnosticText(r);
    return text.find(kOwnedDiscard) != std::string::npos &&
           text.find("non-trivial-dispose") != std::string::npos &&
           text.find("\xC2\xA7""21.9") != std::string::npos;
}

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor the runtime's §28.9 over-release check
// fired.
void didNotCrash(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") ==
                      std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("refcount over-release") == std::string::npos);
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

}  // namespace

// =====================================================================
// REJECT — the issue's own program. RED on r30-c1 (rc 0, a warning only);
// GREEN with the fix (the §21.9 hard error, and NOT the warning as well).
// =====================================================================

// The issue's literal repro: `C.Make()();` where `Make(): () -> owned W` and
// `W` has a destructor. §21.9's escalation is keyed on the RESULT's static
// type, so the discarded owned `W` is the hard error.
//
// The row also pins #6572: the escalation REPLACES the unused-result warning,
// so the warning must be ABSENT from the same compile — exactly one §21.9
// message for this regime.
// per §21.9, §28.3, §28.9 (issue #9245)
BEZEL_TEST(ownership_chain_call_owned_discard_9245,
           chained_call_bare_statement_rejected_spec_21_9) {
    JitResult r = runAot(prog("    C.Make()(); "
                               "    return 0; "));
    // The driver REJECTED the program at a real source location — not a
    // crash, a timeout, or a sanitizer report.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // … and it rejected it for THIS rule: §21.9's non-trivial-dispose hard
    // error, with the §-citation.
    BEZEL_EXPECT_TRUE(rejectedBareOwnedProducer(r));
    // #6572: the escalation replaces the warning; the warning must not also
    // fire at the same site.
    BEZEL_EXPECT_EQ(diagnosticMessageCount(r, kUnusedResult), 0);
}

// =====================================================================
// CONTROL 1 — §21.9's own REMEDY must stay admitted. GREEN on both arms.
//
// `_ := C.Make()();` is §17.13's discard form — the very remedy §21.9's hard
// error names ("bind it (`using var _ := expression;`)"). A fix that rejected
// the CHAINED CALL itself (rather than the discarded RESULT) would reject
// this too, and would also reject the legal `using var x := C.Make()();` —
// so this control is RED under that plausible wrong fix.
//
// The row also pins the disposal: the discard disposes the produced `W`
// (§17.13), so a 200-iteration loop is ASan/LSan-clean.
// per §17.13, §21.9, §28.2 (issue #9245)
BEZEL_TEST(ownership_chain_call_owned_discard_9245,
           chained_call_discard_form_still_accepted_spec_17_13) {
    JitResult r = runAotChecked(prog("    var n := 0; "
                                      "    for var i in 1..200 do "
                                      "      _ := C.Make()(); "
                                      "      n := n + 1; "
                                      "    end; "
                                      "    Console.WriteLine(n); "
                                      "    return 0; "));
    didNotCrash(r, 0, "200\n");
    assertNoLeak(r);
}

// =====================================================================
// CONTROL 2 — the TRIVIAL-dispose twin must stay admitted, and must not
// be freed twice. GREEN on both arms.
//
// `W` here has NO destructor, so it is §28.9-trivial-dispose and the bare
// statement is not a §21.9 hard error: the value is an unbound owned
// temporary whose §28.3.11 disposal codegen already emits for the chain-call
// shape. This row is RED under the plausible wrong fix of ALSO setting §21.9's
// `disposeOwnedValue` reclaim flag for that shape — two frees of one value,
// measured as an ASan heap-use-after-free (the §21.9 free runs first and
// codegen's chain-call disposal then reads the freed object's vtable).
//
// It is also the 200-iteration leak/UAF loop the issue's family needs: one
// allocation per iteration, so a missing or a doubled disposal is loud.
// per §21.9, §28.3.11, §28.9 (issue #9245)
BEZEL_TEST(ownership_chain_call_owned_discard_9245,
           chained_call_trivial_dispose_loop_reclaimed_spec_28_3_11) {
    JitResult r = runAotChecked(trivialProg("    var n := 0; "
                                             "    for var i in 1..200 do "
                                             "      C.Make()(); "
                                             "      n := n + 1; "
                                             "    end; "
                                             "    Console.WriteLine(n); "
                                             "    return 0; "));
    didNotCrash(r, 0, "200\n");
    assertNoLeak(r);
}
