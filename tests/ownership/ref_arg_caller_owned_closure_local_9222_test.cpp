// Ownership — issue #9222: a CALLER-OWNED closure local passed as a `ref`
// ARGUMENT leaked its closure. The local's own scope-end release was never
// armed, because the closure escape walk classified every `ref`-argument
// position as an ESCAPE.
//
// Spec: §22.3 (v0.359) *Parameter modifiers* — the `ref` bullet: "A `ref`
// argument naming a mutable local or writable field is a true alias: callee
// writes are visible through the caller's location immediately." §28.3.8
// (v0.359) — *A `ref` or plain `out` location outlives the call*: the location
// "is caller storage", and "Where the location's declared type is a function
// type it is also a function-typed holder, so a function value stored there is
// admitted only capture-free." §28.9 (v0.359) — a function-typed value's
// "lifetime is managed by the runtime", and the local's own reference must be
// dropped exactly once. §28.14 (v0.359) — the call-bounded default for a
// plain function-typed parameter, whose twin this position is.
//
// THE DEFECT. The issue's own program, verbatim, on r30-c1 @ 9ff2f909f
// (`bezelc --sanitize=address`, LeakSanitizer at exit):
//
//   public static method Peek(ref x: () -> int): int => 0;
//   ...
//   var f: () -> int := lambda() => 2;
//   Console.WriteLine(Peek(ref f));
//
// → stdout `0`, "LeakSanitizer: 64 byte(s) leaked in 1 allocation(s)".
// The BY-VALUE twin (`G(f)`, `G(y: () -> int)`) is CLEAN — the asymmetry is
// the defect.
//
// THE ROOT CAUSE. The closure escape walk decides whether a closure local's
// scope-end release is armed, and it treats a call ARGUMENT as an escape
// unless a premise about the CALLEE holds. The premise for a function-typed
// argument is §28.14's CALL-BOUNDED position — a PLAIN function-typed
// formal. A `ref`/`out` formal is not call-bounded (the callee may WRITE it),
// so the premise declined, the position stayed an ESCAPE, and the local's own
// +1 was never dropped: 64 B per call, unbounded in a loop.
//
// THE FIX. A `ref`/`out` argument position is not an ESCAPE either: §22.3's
// alias and §28.3.8's caller-storage rule make the location the CALLER's, so
// the value in it is the caller's to release, and the local's scope-end
// release must stay armed — exactly as it is for the by-value twin.
//
// THE PREMISE IS NOT THE MODIFIER. §28.14's obligations are NOT enforced for
// a by-ref formal (MEASURED on r30-c1: such a formal may be stored into a
// field, a collection, an `Object`-typed holder, returned, or captured by a
// closure — every shape §28.14 rejects for a PLAIN function-typed formal),
// and a callee that RETAINS the value would make the caller's scope-end
// release an EARLY release. The relief is therefore admitted only where the
// callee's own body keeps nothing of the value past the call.
//
// ORACLE. `runAot` / `runAotChecked` — the AOT path with the
// AddressSanitizer pass over the GENERATED module and LeakSanitizer at exit.
// Every row asserts the program's own stdout AND its exit code, and the leak
// rows LOOP 200 times so that a leak cannot hide under the sanitizer's noise.
//
// MEASURED, `--no-results-cache`, `bezelc --sanitize=address`, 200 iterations
// per program (the probe matrix in the lane's handback):
//
//   program                                     r30-c1 @ 9ff2f909f   with the fix
//   the callee IGNORES x (the issue's repro)   64 B / 1 alloc        CLEAN
//   the callee CALLS x                          64 B / 1 alloc        CLEAN
//   the callee FORWARDS x by ref                64 B / 1 alloc        CLEAN
//   the callee CAPTURES x in a local closure    64 B / 1 alloc        CLEAN
//   the callee REASSIGNS x                      128 B / 2 allocs      128 B / 2 (unchanged)
//   the callee RETAINS x in a field             64 B / 1 alloc        64 B / 1 (unchanged)
//
// The REASSIGN row is unchanged, and deliberately so: the callee's displacing
// store REPLACES the caller's value, and the reference the displaced value
// carried is #9111's subject ("the caller mints the reference a function-typed
// `ref`/`out` slot lacks") — a separate commit, not this one. The RETAIN row
// is unchanged because the callee keeps the value past the call, and the
// relief must not fire there (a control below).

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"         // runAot / runAotChecked
#include "support/ownership_support.hpp"
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

// The sanitizer leg ran and found nothing: no AddressSanitizer error (a
// use-after-free would be one) and no LeakSanitizer report.
void assertNoSanitizerError(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output.
void expectOutput(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// The leak the RETAIN control documents: the caller's own +1 on the closure
// is NOT released (the callee kept the value past the call), so the local
// leaks one 64-byte closure cell per iteration — the SAFE direction, and the
// bound the control pins.
//
// NOTE: the checked leg reports the leak as a FINDING (the leak oracle is
// strict), so `r.ran` is false BY DESIGN on this row — the leak is the
// expected result, not a failure. What must NOT happen is an
// AddressSanitizer error: an early release of the closure the field still
// points at would be a heap-use-after-free.
void expectBoundedResidualLeak(const JitResult& r, unsigned long long maxBytes,
                               unsigned long long maxAllocs) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs
                  << " allocation(s) / " << leakedBytes
                  << " byte(s) leaked (bound " << maxAllocs << " / "
                  << maxBytes << ")\n";
    BEZEL_EXPECT_TRUE(sawSummary);
    BEZEL_EXPECT_TRUE(leakedAllocs <= maxAllocs);
    BEZEL_EXPECT_TRUE(leakedBytes <= maxBytes);
    // A use-after-free would be an AddressSanitizer error, not a leak.
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// =====================================================================
// (A1) The issue's verbatim repro: a caller-owned closure local passed as
// a `ref` argument to a callee that never touches the formal. 200
// iterations, each a fresh closure.
// RED on the base: 64 B leaked in 1 allocation (12800 B / 200 iterations).
// =====================================================================
const char* kIssueRepro = R"BZ(
module M;
import System;
public class Program
  public static method Peek(ref x: () -> int): int => 0;
  public static method Main(): int
    var sum := 0;
    for var i in 0 ..< 200 do
      var f: () -> int := lambda() => 2;
      sum := sum + Peek(ref f);
      sum := sum + f();
    end for;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
)BZ";

// =====================================================================
// (A2) The callee CALLS the formal (§22.3: "its referent keeps the
// caller's other names for the whole call, so it is read at its declared
// type"). The caller's local must still be released at its scope end.
// RED on the base: 64 B leaked in 1 allocation.
// =====================================================================
const char* kCalleeCallsX = R"BZ(
module M;
import System;
public class Program
  public static method Peek(ref x: () -> int): int => x();
  public static method Main(): int
    var sum := 0;
    for var i in 0 ..< 200 do
      var f: () -> int := lambda() => 2;
      sum := sum + Peek(ref f);
    end for;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
)BZ";

// =====================================================================
// (A3) The callee FORWARDS the formal to a second by-ref formal
// (§28.3.8: the location is caller storage, so the forwarded callee
// writes the SAME location and the value stays in the caller's scope).
// RED on the base: 64 B leaked in 1 allocation.
// =====================================================================
const char* kCalleeForwardsX = R"BZ(
module M;
import System;
public class Program
  public static method Peek2(ref x: () -> int): int => x();
  public static method Peek(ref x: () -> int): int return Peek2(ref x); end Peek;
  public static method Main(): int
    var sum := 0;
    for var i in 0 ..< 200 do
      var f: () -> int := lambda() => 2;
      sum := sum + Peek(ref f);
    end for;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
)BZ";

// =====================================================================
// (A4) The callee CAPTURES the formal in a closure of its own that does
// not escape it (§28.14: a callback may be "bound to locals"; the closure
// borrows the caller's storage and is disposed at the callee's own scope
// end, before the caller's scope-end release).
// RED on the base: 64 B leaked in 1 allocation.
// =====================================================================
const char* kCalleeCapturesX = R"BZ(
module M;
import System;
public class Program
  public static method Peek(ref x: () -> int): int
    var cb := lambda() => x();
    return cb();
  end Peek;
  public static method Main(): int
    var sum := 0;
    for var i in 0 ..< 200 do
      var f: () -> int := lambda() => 2;
      sum := sum + Peek(ref f);
    end for;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
)BZ";

// =====================================================================
// (B1) CONTROL — the callee RETAINS the value past the call (a store into
// a field of its own, §28.14's sink the by-ref formal is NOT held to).
// The relief must NOT fire here: an early release would free the cell the
// field still points at, and the field's read below would be a
// heap-use-after-free.
//
// GREEN on the base and with the fix — it pins the BOUNDARY, and it is red
// under the plausible wrong fix this commit names (a relief that fires at
// every by-ref argument position, with no premise about the callee).
// =====================================================================
const char* kRetainingCallee = R"BZ(
module M;
import System;
public class Holder
  public cb: () -> int := lambda() => 0;
  public constructor()
  end constructor;
end Holder;
public class Program
  public static method Stash(ref x: () -> int, h: Holder): int
    h.cb := x;
    return 0;
  end Stash;
  public static method Main(): int
    var sum := 0;
    for var i in 0 ..< 200 do
      var f: () -> int := lambda() => 2;
      var h := new Holder();
      sum := sum + Stash(ref f, h);
      sum := sum + h.cb();
    end for;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
)BZ";

// =====================================================================
// (B2) CONTROL — the BY-VALUE twin of (A1): the same closure local passed
// to a plain function-typed formal (§28.14's call-bounded default). It is
// CLEAN on the base and with the fix; it pins that the by-value path keeps
// its own accounting (one release, no double release).
// =====================================================================
const char* kByValueTwin = R"BZ(
module M;
import System;
public class Program
  public static method G(y: () -> int): int => y();
  public static method Main(): int
    var sum := 0;
    for var i in 0 ..< 200 do
      var f: () -> int := lambda() => 2;
      sum := sum + G(f);
    end for;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
)BZ";

}  // namespace

// (A1) The issue's verbatim repro: the callee never touches the formal.
// RED on the base: 64 B leaked per call (12800 B over 200 iterations).
BEZEL_TEST(ownership_ref_arg_caller_owned_closure_local_9222,
           issue_repro_ref_arg_ignores_x_spec_22_3) {
    auto r = runAot(kIssueRepro);
    expectOutput(r, 0, "400\n");
    auto c = runAotChecked(kIssueRepro);
    expectOutput(c, 0, "400\n");
    assertNoLeak(c);
}

// (A2) The callee CALLS the formal through the ref slot.
// RED on the base: 64 B leaked per call.
BEZEL_TEST(ownership_ref_arg_caller_owned_closure_local_9222,
           ref_arg_callee_calls_x_spec_22_3) {
    auto r = runAot(kCalleeCallsX);
    expectOutput(r, 0, "400\n");
    auto c = runAotChecked(kCalleeCallsX);
    expectOutput(c, 0, "400\n");
    assertNoLeak(c);
}

// (A3) The callee FORWARDS the formal to a second by-ref formal.
// RED on the base: 64 B leaked per call.
BEZEL_TEST(ownership_ref_arg_caller_owned_closure_local_9222,
           ref_arg_callee_forwards_x_by_ref_spec_28_3_8) {
    auto r = runAot(kCalleeForwardsX);
    expectOutput(r, 0, "400\n");
    auto c = runAotChecked(kCalleeForwardsX);
    expectOutput(c, 0, "400\n");
    assertNoLeak(c);
}

// (A4) The callee CAPTURES the formal in a closure of its own that does
// not escape the callee.
// RED on the base: 64 B leaked per call.
BEZEL_TEST(ownership_ref_arg_caller_owned_closure_local_9222,
           ref_arg_callee_captures_x_in_local_closure_spec_27_5) {
    auto r = runAot(kCalleeCapturesX);
    expectOutput(r, 0, "400\n");
    auto c = runAotChecked(kCalleeCapturesX);
    expectOutput(c, 0, "400\n");
    assertNoLeak(c);
}

// (B1) CONTROL — the callee retains the value past the call. The relief
// must not fire: an early release would be a heap-use-after-free. The
// documented residual is the caller's own +1, one 64-byte cell per
// iteration.
BEZEL_TEST(ownership_ref_arg_caller_owned_closure_local_9222,
           ref_arg_retaining_callee_keeps_the_value_alive_spec_28_9) {
    auto r = runAot(kRetainingCallee);
    expectOutput(r, 0, "400\n");
    auto c = runAotChecked(kRetainingCallee);
    // 200 iterations x one 64-byte closure cell.
    expectBoundedResidualLeak(c, /*maxBytes=*/12800, /*maxAllocs=*/200);
}

// (B2) CONTROL — the by-value twin stays clean.
BEZEL_TEST(ownership_ref_arg_caller_owned_closure_local_9222,
           by_value_twin_stays_clean_spec_28_14) {
    auto r = runAot(kByValueTwin);
    expectOutput(r, 0, "400\n");
    auto c = runAotChecked(kByValueTwin);
    expectOutput(c, 0, "400\n");
    assertNoLeak(c);
}
