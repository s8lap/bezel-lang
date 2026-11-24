// Ownership regression test for issue #9009 — §28.3.11 *Unbound owned
// temporaries* / §35.4 *The classification* (spec v0.359).
//
// #9009 — `[codegen] an unbound owned temporary of an OWNING-CHOICE type used
// as the receiver of a further call is never disposed (§28.3.11, v0.359)`.
//
// §28.3.11 (v0.359): "An owned value may also be produced as an **unbound
// temporary**: the result of an `owned`-returning call (or other owned
// producer) that is not bound to any name, not `move`d, and not returned …,
// but used only by *borrowing* operations within the expression — **as the
// receiver of a further call**, as a non-`owned` argument, as an operator
// operand, as a member or index access, or as the scrutinee of a `case` …;
// Such a temporary is disposed at the **end of the full expression** in which
// it is produced, after every use of it within that expression has
// completed."
//
// §28.3.11's *Exception-path discharge* (v0.359): "When the full expression is
// instead exited by an **exception** — an operand or argument evaluated later
// throws before the temporary's transfer or consumption point — every already-
// produced, not-yet-consumed unbound owned temporary is disposed during
// unwinding by the same implicit `try`/`finally` mechanism as named locals …
// each temporary's disposal is established at its production, so the pass runs
// in reverse order of production on the establishment-ordered cleanup stack".
//
// WHAT THIS FILE PINS. The §28.3.11 exception-path discharge of an
// OWNING-CHOICE receiver temporary must dispose the temporary EXACTLY ONCE,
// even when the scope it was established in is unwound MORE THAN ONCE.
//
// The shape: a receiver temporary that is produced on ONE path of a loop
// (`(i = 0) and h.Make().Boom()` — the `and` short-circuits, so the receiver
// is produced on the first iteration only) and a LATER throw in the SAME
// scope (`h.Arg()`), which unwinds that scope again. The unwind pass of the
// scope re-reads the temporary's slot on every throw that leaves the scope, so
// the slot must be EMPTY after the first discharge.
//
// MEASURED (200 iterations, under ASan/LSan):
//
//   arm                                r30-c1 (base)      dc8fbc750 (rejected tip)
//   stdout                             `N=0 Made=1`       ASan: heap-use-after-free
//   LeakSanitizer                      16 byte(s) in 1    — (the run faults first)
//                                      allocation(s)
//   this fix                           `N=1 Made=1`,      `N=1 Made=1`, clean
//                                      clean
//
// On the base the temporary is never disposed at all (the issue's leak); on the
// rejected tip it is disposed, but the slot is not cleared afterwards, so the
// SECOND unwind of the same scope disposes the already-freed payload again —
// a heap-use-after-free. `N` is the number of DISPOSALS the counting
// destructor saw: 1 is "exactly once", 0 is "never", and a second disposal
// faults before the program can print.
//
// THE ORACLE. A COUNTING destructor gives one increment per disposal, so a
// suppressed disposal (a leak) is distinguishable from a doubled one (a double
// free) — the same oracle the neighbouring §17.13 discard rows and the
// sibling #8959 rows use. The first row runs on `runAot` (the counting
// destructor alone separates the two: the leak prints `0`, the fix prints
// `1`); the second row runs on `runAotChecked`, the ASan/LSan oracle, and
// additionally asserts that LeakSanitizer counted nothing — `runAot` does not
// leak-check at all, so the byte/allocation count needs the instrumented
// oracle, and `checkedToolchainSkipped` makes a missing sanitizer toolchain
// LOUD instead of a silent pass.

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
// OWN disposals (`N`) — so the program's output says exactly how many
// temporaries were produced and how many were disposed, which is the whole
// point of the row.
//
// `OChoice` is an OWNING choice (§35.4: a choice with an `owned`-marked
// payload) whose `Text` case holds the `owned` payload; `Res` is the CLASS
// twin of that payload. `Boom` throws from the receiver's own call, `Arg`
// throws from a LATER statement of the same scope.
const char* kDecls =
    "class Res "
    "  public static N: int := 0; "
    "  public static Made: int := 0; "
    "  public constructor() Res.Made := Res.Made + 1; end constructor; "
    "  destructor Res.N := Res.N + 1; end destructor; "
    "  public method Id(): int => 7; "
    "  public method Boom(): int "
    "    throw new InvalidOperationException(\"boom\"); "
    "  end Boom; "
    "end Res; "
    "choice OChoice Text(owned r: Res), Empty; "
    "  public method Count(): int => "
    "    (case this of Text(r) => r.Id(), Empty => 0); "
    "  public method Boom(): int "
    "    throw new InvalidOperationException(\"boom\"); "
    "  end Boom; "
    "end OChoice; "
    "class H "
    "  public constructor() end constructor; "
    "  public method Make(): OChoice return OChoice.Text(new Res()); end Make; "
    "  public method Arg(): int "
    "    throw new InvalidOperationException(\"argboom\"); "
    "  end Arg; "
    "end H; ";

// The loop body: 200 iterations. On the FIRST iteration the `and`'s left
// operand is true, so the receiver temporary `h.Make()` IS produced and its
// own call `Boom()` throws — the §28.3.11 exception-path discharge must
// dispose the temporary while unwinding. On every LATER iteration the `and`
// short-circuits (the left operand is false), so the receiver is NOT produced
// at all, and the LATER statement `h.Arg()` throws — unwinding the SAME scope
// a second time, which re-reads the temporary's slot.
//
// `h.Arg()` is reached only on the later iterations (the first iteration's
// `Boom()` throws first), so the scope is unwound 200 times and the
// temporary's slot is re-read 199 times after the first discharge.
const char* kLoop =
    "var i := 0; "
    "var h := new H(); "
    "while i < 200 do "
    "  try "
    "    var b := (i = 0) and h.Make().Boom() > 0; "
    "    h.Arg(); "
    "  catch InvalidOperationException => i := i + 0; "
    "  end try; "
    "  i := i + 1; "
    "end while; "
    "Console.WriteLine($\"N={Res.N} Made={Res.Made}\");";

// The CLASS twin of the same shape: a fresh owned-CLASS producer used only as
// the receiver of a further call (§28.3.11's "as the receiver of a further
// call" position, the family the compiler has implemented since #2816). Its
// expectation is the SAME — the receiver is disposed exactly once, on the
// first iteration's unwind, and the 199 later unwinds dispose nothing.
const char* kClassTwinLoop =
    "var i := 0; "
    "var h := new H(); "
    "while i < 200 do "
    "  try "
    "    var b := (i = 0) and new Res().Boom() > 0; "
    "    h.Arg(); "
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

}  // namespace

// --- #9009: the owning-choice receiver temporary's EXCEPTION-PATH DISCHARGE --

BEZEL_TEST(ownership_owning_choice_receiver_temp_unwind_stale_slot_9009,
           spec_28_3_11_the_receiver_temporary_is_disposed_exactly_once_when_the_scope_unwinds_twice) {
    // The owning-choice receiver temporary, produced on the first iteration
    // and disposed by the §28.3.11 exception-path discharge, with the same
    // scope unwound again by a later throw. The counting destructor must run
    // EXACTLY once — `N=1` — and the program must not fault.
    //
    // Pre-fix (r30-c1): `N=0 Made=1` — the temporary is never disposed at all
    // (the issue's leak), and LeakSanitizer reports "16 byte(s) leaked in 1
    // allocation(s)".
    // On the rejected tip (dc8fbc750): AddressSanitizer: heap-use-after-free —
    // the discharge disposes the payload, and the LATER unwind of the same
    // scope disposes the already-freed payload again.
    // With the fix: `N=1 Made=1`, clean.
    didNotCrash(runAotProgram(kLoop), 0, std::string("N=1 Made=1\n"));
}

BEZEL_TEST(ownership_owning_choice_receiver_temp_unwind_stale_slot_9009,
           spec_28_3_11_the_same_program_under_the_leak_checking_oracle) {
    // The same program on the ASan/LSan oracle: the disposal count must be 1
    // AND LeakSanitizer must count nothing. A DOUBLE disposal (the discharge
    // firing twice) shows up as a heap-use-after-free / an over-release, which
    // `didNotCrash` rejects, and a SUPPRESSED disposal as a leak, which
    // `assertNoLeak` rejects.
    //
    // Pre-fix (r30-c1): `N=0 Made=1` and "16 byte(s) leaked in 1
    // allocation(s)".
    // On the rejected tip (dc8fbc750): AddressSanitizer: heap-use-after-free.
    // With the fix: `N=1 Made=1`, nothing leaked.
    expectClean(runAotCheckedProgram(kLoop), 0, std::string("N=1 Made=1\n"));
}

BEZEL_TEST(ownership_owning_choice_receiver_temp_unwind_stale_slot_9009,
           spec_28_3_11_the_class_twin_of_the_same_program_is_unchanged) {
    // THE BOUNDARY CONTROL (green on the base). The CLASS twin of the same
    // shape: a fresh owned-CLASS producer used only as the receiver of a
    // further call. §28.3.11's boundary at this position was already
    // implemented for the class family, so this row is green BEFORE the fix and
    // must stay green after it — the fix must not change the class family's
    // behaviour.
    //
    // It is red under a plausible wrong fix: one that re-uses the receiver's
    // OWN spill slot for the class family's exception-edge discharge (the
    // mistake the owning-choice sibling made) would double-dispose the class
    // receiver here, exactly as it double-disposes the choice receiver in the
    // row above.
    //
    // Pre-fix: `N=1 Made=1` (green).
    didNotCrash(runAotProgram(kClassTwinLoop), 0, std::string("N=1 Made=1\n"));
}
