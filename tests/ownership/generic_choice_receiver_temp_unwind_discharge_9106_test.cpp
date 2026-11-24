// Ownership regression test for issue #9106 — §28.3 *Exception-path
// discharge* (spec v0.359).
//
// #9106 — `[ownership][leak] an unbound GENERIC owning-choice receiver
// temporary whose called method throws is never disposed on the unwind edge:
// `h.MakeW().Boom()` with `Wrap<Res>` (§28.3.11 Exception-path discharge,
// v0.359)`.
//
// §28.3 *Exception-path discharge* (v0.359): "When the full expression is
// instead exited by an **exception** — an operand or argument evaluated later
// throws before the temporary's transfer or consumption point — every already-
// produced, not-yet-consumed unbound owned temporary is disposed during
// unwinding by the same implicit `try`/`finally` mechanism as named locals …".
//
// §28.3.11 (v0.359) names the position the issue's repro occupies: an unbound
// owned temporary "used only by *borrowing* operations within the expression —
// **as the receiver of a further call**". The receiver of `h.MakeW().Boom()`
// is such a temporary, and the `Boom()` call throws.
//
// THE DEFECT THIS FILE PINS. The leak is NOT specific to the receiver
// temporary and NOT specific to owning choices: it is the CALLER's §28.3
// exception-path discharge that never ran. A call to a method of a GENERIC type
// that throws left every live scope of the CALLING scope undisposed, because
// the generic call path's post-call pending check ran with an EMPTY unwind
// range (see the fix's commit message for the measurement). The issue's own
// program is the narrowest shape that shows it — an unbound owning-choice
// receiver temporary whose payload's destructor never ran.
//
// MEASURED (the issue's own program, 200 iterations, `--probe --asan`):
//
//   arm                             stdout            LeakSanitizer
//   r30-c1 (base)                   `N=0 Made=200`    3200 byte(s) in 200 allocation(s)
//   this fix                        `N=200 Made=200`  clean
//
// `N` is the number of DISPOSALS the counting destructor saw: 200 is "every
// temporary disposed exactly once", 0 is "never disposed" (the issue's leak).
// A DOUBLE disposal would print `N=400` and fault under ASan before the
// program could print.
//
// THE ORACLE. A COUNTING destructor gives one increment per disposal, so a
// suppressed disposal (a leak) is distinguishable from a doubled one (a double
// free) — the same oracle the neighbouring §28.3.11 (#9009) rows and the
// sibling #8959 rows use. The first row runs on `runAot` (the counting
// destructor alone separates the two: the leak prints `0`, the fix prints
// `200`); the second row runs on `runAotChecked`, the ASan/LSan oracle, and
// additionally asserts that LeakSanitizer counted nothing — `runAot` does not
// leak-check at all, so the byte/allocation count needs the instrumented
// oracle, and `checkedToolchainSkipped` makes a missing sanitizer toolchain
// LOUD instead of a silent pass.
//
// THE THIRD ROW pins the SAME defect at the position the issue's framing does
// not mention: a BOUND owning-choice local of a generic type, declared in the
// scope the throwing generic call unwinds. The local's payload is an owned
// value like the temporary's, and §28.3's discharge covers it by the same
// sentence ("the same implicit `try`/`finally` mechanism as named locals"), so
// it must be disposed exactly once too.
//
// THE FOURTH ROW pins the SECOND shape the defect reaches: a GENERIC METHOD
// (a type-parameterized method on a NON-generic class), whose body throws.
// The compiler monomorphises such a call at the call site exactly as it does
// a method of a generic type, so the caller's discharge was lost there too —
// MEASURED on r30-c1: the `using var` local in the `try` body was never
// disposed (`N=0 Made=1`, 16 byte(s) leaked in 1 allocation). The row uses a
// `using var` (not an unbound temporary) so that it pins the DISCHARGE of the
// caller's own scope — the shape the issue's own repro does not reach.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
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

// The program ran to completion: it compiled, it exited normally with the
// program's own exit code and the exact expected output, and neither
// AddressSanitizer nor the runtime's §28.9 over-release check fired.
void didNotCrash(const JitResult& r, int exitCode, const std::string& out) {
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

void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    didNotCrash(r, exitCode, out);
    assertNoLeak(r);
}

// The shared declarations. `Res` counts its OWN constructions (`Made`) and its
// OWN disposals (`N`) — so the program's output says exactly how many payloads
// were produced and how many were disposed, which is the whole point of the
// row.
//
// `Wrap<T>` is the GENERIC owning choice (§35.4: a choice with an `owned`-
// marked payload) of the issue's own repro; its `Has` case holds the `owned`
// payload of the instantiated type. `Boom` throws from the receiver's own
// call. `OChoice` is the NON-generic twin (the shape the issue says is
// already correct), kept here so the same program's two halves can be
// compared by a reader of this file.
const char* kDecls =
    "class Res "
    "  public static N: int := 0; "
    "  public static Made: int := 0; "
    "  public constructor() Res.Made := Res.Made + 1; end constructor; "
    "  destructor Res.N := Res.N + 1; end destructor; "
    "  public method Id(): int => 7; "
    "end Res; "
    "choice Wrap<T> Has(owned v: T), None; "
    "  public method Count(): int => "
    "    (case this of Has(v) => 1, None => 0); "
    "  public method Boom(): int "
    "    throw new InvalidOperationException(\"boom\"); "
    "  end Boom; "
    "end Wrap; "
    "class H "
    "  public constructor() end constructor; "
    "  public method MakeW(): Wrap<Res> "
    "    return Wrap<Res>.Has(new Res()); "
    "  end MakeW; "
    "end H; ";

// The issue's own program, looped 200 times: the unbound owning-choice
// receiver temporary `h.MakeW()` is produced, its own call `Boom()` throws,
// and the §28.3 exception-path discharge must dispose the temporary while
// unwinding. `Res.Made` counts the productions (200) and `Res.N` the
// disposals (200).
const char* kReceiverTempLoop =
    "var i := 0; "
    "var h := new H(); "
    "while i < 200 do "
    "  try "
    "    var z := h.MakeW().Boom(); "
    "  catch InvalidOperationException => i := i + 0; "
    "  end try; "
    "  i := i + 1; "
    "end while; "
    "Console.WriteLine($\"N={Res.N} Made={Res.Made}\");";

// The same defect at the BOUND-LOCAL position: `h.MakeW()` is bound to a
// local `w` in the scope the throwing generic call unwinds, so the local's
// `owned` payload is disposed by the same §28.3 sentence ("the same implicit
// `try`/`finally` mechanism as named locals").
const char* kBoundLocalLoop =
    "var i := 0; "
    "var h := new H(); "
    "while i < 200 do "
    "  try "
    "    var w := h.MakeW(); "
    "    w.Boom(); "
    "  catch InvalidOperationException => i := i + 0; "
    "  end try; "
    "  i := i + 1; "
    "end while; "
    "Console.WriteLine($\"N={Res.N} Made={Res.Made}\");";

JitResult runAotProgram(const std::string& body) {
    return runAot(std::string("module M; ") + kDecls +
                  " class Program public static method Main() " + body +
                  " end Main; end Program;");
}

JitResult runAotCheckedProgram(const std::string& body) {
    return runAotChecked(std::string("module M; ") + kDecls +
                         " class Program public static method Main() " + body +
                         " end Main; end Program;");
}

// The FOURTH row's own declarations: a type-parameterized method on a
// NON-generic class. `ThrowB<T>` throws from its block body; the compiler
// monomorphises the call at the call site (`C.ThrowB<Res>()`), so it reaches
// the same inlined generic path as `Wrap<Res>.Boom()` does.
const char* kGenericMethodDecls =
    "class Res "
    "  public static N: int := 0; "
    "  public static Made: int := 0; "
    "  public constructor() Res.Made := Res.Made + 1; end constructor; "
    "  destructor Res.N := Res.N + 1; end destructor; "
    "end Res; "
    "class C "
    "  public constructor() end constructor; "
    "  public static method ThrowB<T>(): int "
    "    throw new InvalidOperationException(\"boom\"); "
    "  end ThrowB; "
    "end C; ";

// The `using var` local in the `try` body must be disposed when the
// monomorphised generic method throws: §28.3's *Exception-path discharge*
// covers "owned locals ... that are live when an exception propagates out of
// their enclosing block".
const char* kGenericMethodLoop =
    "try "
    "  using var keep := new Res(); "
    "  var z := C.ThrowB<Res>(); "
    "catch InvalidOperationException => Res.N := Res.N + 0; "
    "end try; "
    "Console.WriteLine($\"N={Res.N} Made={Res.Made}\");";

}  // namespace

// --- #9106: the caller's §28.3 exception-path discharge at a GENERIC call ---

BEZEL_TEST(ownership_generic_choice_receiver_temp_unwind_discharge_9106,
           spec_28_3_the_generic_owning_choice_receiver_temporary_is_disposed_when_the_callee_throws) {
    // The issue's own program: an unbound owning-choice receiver temporary of
    // a GENERIC choice whose called method throws. The counting destructor must
    // run EXACTLY once per production — `N=200 Made=200` — and the program
    // must not fault.
    //
    // Pre-fix (r30-c1): `N=0 Made=200` — the temporary is never disposed at
    // all (the issue's leak), and LeakSanitizer reports "3200 byte(s) leaked
    // in 200 allocation(s)".
    // With the fix: `N=200 Made=200`, clean.
    didNotCrash(runAotProgram(kReceiverTempLoop), 0,
                std::string("N=200 Made=200\n"));
}

BEZEL_TEST(ownership_generic_choice_receiver_temp_unwind_discharge_9106,
           spec_28_3_the_same_program_under_the_leak_checking_oracle) {
    // The same program on the ASan/LSan oracle: the disposal count must be 200
    // AND LeakSanitizer must count nothing. A DOUBLE disposal (the discharge
    // firing twice) shows up as a heap-use-after-free / an over-release, which
    // `didNotCrash` rejects, and a SUPPRESSED disposal as a leak, which
    // `assertNoLeak` rejects.
    //
    // Pre-fix (r30-c1): `N=0 Made=200` and "3200 byte(s) leaked in 200
    // allocation(s)".
    // With the fix: `N=200 Made=200`, nothing leaked.
    expectClean(runAotCheckedProgram(kReceiverTempLoop), 0,
                std::string("N=200 Made=200\n"));
}

BEZEL_TEST(ownership_generic_choice_receiver_temp_unwind_discharge_9106,
           spec_28_3_the_generic_owning_choice_local_is_disposed_when_a_generic_callee_throws) {
    // The SAME defect at the BOUND-LOCAL position: `h.MakeW()` bound to a
    // local `w`, whose `owned` payload is a named local's payload — §28.3's
    // discharge covers it "by the same implicit `try`/`finally` mechanism as
    // named locals". The counting destructor must run exactly once per
    // production.
    //
    // Pre-fix (r30-c1): `N=0 Made=200` — the local's payload is never
    // disposed (the caller's scope was never discharged), and LeakSanitizer
    // reports "3200 byte(s) leaked in 200 allocation(s)".
    // With the fix: `N=200 Made=200`, clean.
    expectClean(runAotCheckedProgram(kBoundLocalLoop), 0,
                std::string("N=200 Made=200\n"));
}

BEZEL_TEST(ownership_generic_choice_receiver_temp_unwind_discharge_9106,
           spec_28_3_the_caller_s_using_var_is_disposed_when_a_generic_method_throws) {
    // The SECOND shape the defect reaches: a GENERIC METHOD (a
    // type-parameterized method on a non-generic class) whose body throws.
    // The compiler monomorphises the call at the call site, so it reaches the
    // same inlined generic path as `Wrap<Res>.Boom()` does, and the caller's
    // §28.3 discharge was lost there too. The `using var keep` in the `try`
    // body must be disposed exactly once.
    //
    // Pre-fix (r30-c1): `N=0 Made=1` — the local was never disposed, and
    // LeakSanitizer reports "16 byte(s) leaked in 1 allocation(s)".
    // With the fix: `N=1 Made=1`, clean.
    expectClean(runAotChecked(std::string("module M; ") + kGenericMethodDecls +
                              " class Program public static method Main() " +
                              kGenericMethodLoop + " end Main; end Program;"),
                0, std::string("N=1 Made=1\n"));
}
