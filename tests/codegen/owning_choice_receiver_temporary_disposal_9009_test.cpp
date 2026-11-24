// Codegen regression test for issue #9009 — §28.3.11 *Unbound owned
// temporaries* / §35.4 *The classification* / §21.9 *The statement form of
// expressions* (spec v0.359).
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
// The CLASS family implements that position (a fresh owned-class producer used
// only as a receiver is disposed after the call); the OWNING-CHOICE family did
// not: `h.Make().Count()` — `Make` returning an owning-classified choice
// (§35.4) — left the choice's active case's `owned` payload owned by NOTHING,
// and every iteration leaked it.
//
// THE ORACLE. A COUNTING destructor gives one increment per disposal, so a
// suppressed disposal (a leak) is distinguishable from a doubled one (a double
// free) — the same oracle the neighbouring §17.13 discard rows use
// (`owning_choice_discard_and_bang_field_store_7861_7858`) and the same one the
// sibling #8959 rows use. The first row runs on `runAot` (the counting
// destructor alone already separates the two: the leak prints `0`, the fix
// prints `200`); the second row runs on `runAotChecked`, the ASan/LSan
// oracle, and additionally asserts that LeakSanitizer counted nothing —
// `runAot` does not leak-check at all, so the byte/allocation count needs the
// instrumented oracle, and `checkedToolchainSkipped` makes a missing
// sanitizer toolchain LOUD instead of a silent pass.
//
// MEASURED on the base (r30-c1 @ 2e44cab96), 200 iterations:
//   `h.Make().Count();`  → the count printed `0` (the destructor never ran)
//                          and LeakSanitizer reported "3200 byte(s) leaked in
//                          200 allocation(s)", allocated from `bz$M.H$Make`;
//   `new Resource().Count();` → the count printed `200` and nothing leaked —
//                          the CLASS twin of the same shape, the issue's own
//                          control, which the fix must leave alone.
//
// The rows below are the issue's own repro and its class twin. The class twin
// is the suite's boundary control: it is GREEN on the base (the class family
// already implements the §28.3.11 boundary at this position) and stays green
// after the fix — it is red under a fix that widened the reclaim to shapes
// the class family never reclaimed.

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

// The issue's own program, VERBATIM in shape: a counting `Resource` with a
// destructor, the owning choice `Bundle` whose `Ready` case holds the
// `owned` payload, a `Holder` whose `Make` mints one, and the choice's own
// `Count` method — the receiver of the further call.
//
// `Bundle.Count()` reads the static counter through the choice's active case,
// so it reads the payload the temporary owns: the count is exactly the number
// of payloads DISPOSED.
const char* kDecls =
    "class Resource "
    "  public static Disposed: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Resource.Disposed := Resource.Disposed + 1; end destructor; "
    "  public method Count(): int => Resource.Disposed; "
    "end Resource; "
    "choice Bundle Ready(owned r: Resource), Empty; "
    "  public method Count(): int => "
    "    (case this of Ready(r) => Resource.Disposed, Empty => 0); "
    "end Bundle; "
    "class Holder "
    "  public constructor() end constructor; "
    "  public method Make(): Bundle return Bundle.Ready(new Resource()); end Make; "
    "end Holder; ";

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

// The issue's 200-iteration loop, with `h.Make().Count()` — the owning-choice
// temporary used as the RECEIVER of a further call.
const char* kReceiverLoop =
    "var i := 0; "
    "while i < 200 do "
    "  var h := new Holder(); "
    "  h.Make().Count(); "
    "  i := i + 1; "
    "end while; "
    "Console.WriteLine($\"{Resource.Disposed}\");";

}  // namespace

// --- #9009: the owning-choice temporary at the RECEIVER position ------------

BEZEL_TEST(codegen_owning_choice_receiver_temporary_disposal_9009,
           spec_28_3_11_an_owning_choice_temporary_used_as_the_receiver_of_a_further_call_is_disposed) {
    // The issue's literal repro: `h.Make().Count();` inside a 200-iteration
    // loop. Each iteration mints one `Resource` (inside the choice's `Ready`
    // payload) and the temporary that owns it dies at the end of the
    // statement, so the counting destructor must run 200 times.
    //
    // Pre-fix: `0` — the choice was never disposed, so the payload was never
    // disposed, and LeakSanitizer reported "3200 byte(s) leaked in 200
    // allocation(s)".
    didNotCrash(runAotProgram(kReceiverLoop), 0, std::string("200\n"));
}

BEZEL_TEST(codegen_owning_choice_receiver_temporary_disposal_9009,
           spec_28_3_11_the_same_statement_under_the_leak_checking_oracle) {
    // The same program on the ASan/LSan oracle: the count must be 200 AND
    // LeakSanitizer must count nothing. A DOUBLE disposal (the fix firing
    // twice) would show up as a double free / an over-release, which
    // `didNotCrash` rejects.
    //
    // Pre-fix: `0` and "3200 byte(s) leaked in 200 allocation(s)".
    expectClean(runAotCheckedProgram(kReceiverLoop), 0, std::string("200\n"));
}

BEZEL_TEST(codegen_owning_choice_receiver_temporary_disposal_9009,
           spec_28_3_11_the_class_twin_of_the_same_statement_is_unchanged) {
    // THE BOUNDARY CONTROL (green on the base). The issue's own control: the
    // CLASS twin of the same shape — a fresh owned-class producer used only as
    // the receiver of a further call. §28.3.11's boundary at this position
    // was already implemented for the class family, so this row is green
    // BEFORE the fix and must stay green after it: the fix must not change
    // the class family's behaviour, and must not widen the class reclaim to
    // shapes the class family never reclaimed.
    //
    // Pre-fix: `200` (green).
    didNotCrash(
        runAotProgram("var i := 0; "
                      "while i < 200 do "
                      "  new Resource().Count(); "
                      "  i := i + 1; "
                      "end while; "
                      "Console.WriteLine($\"{Resource.Disposed}\");"),
        0, std::string("200\n"));
}
