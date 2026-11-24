// Regression test for issue #8988 — the consume-site freeze rejected a `move`
// that comes AFTER the last use of a closure DECLARED INSIDE THE SAME LOOP
// BODY. The rejection was a FALSE one: the closure is a fresh binding on every
// iteration, so the back edge of the loop reaches a NEW closure (holding a NEW
// borrow of the NEXT iteration's owner), not a later use of the borrow the
// consume site is about.
//
// THE DEFECT (MEASURED on the base r30-c1 @ cd10f4022, `bezelc`):
//
//   module M;
//   public class Res
//     public N: int := 7;
//     public constructor() end constructor;
//   end Res;
//   public class Sink
//     public static method Take(owned r: Res): int => 1;
//   end Sink;
//   public class Program
//     public static method Main(): int
//       var r := 0;
//       for var j in 0..<3 do
//         var a := new Res();
//         var f := lambda(): int => a.N;
//         r := r + f();
//         r := r + Sink.Take(move a);
//       end;
//       Console.WriteLine($"{r}");
//       return 0;
//     end Main;
//   end Program;
//
//   probes/repro_8988.bzl:16:31: cannot consume `a` here: the borrow held by
//   `f` is still live — its later use would read freed memory (consume-site
//   freeze, §28.3/§28.7/§30.14 v0.180)
//
// The freeze's liveness query closed the textual "used-after" search over
// loops: a use textually BEFORE the consume but inside the same loop body
// re-executes after the consume on the next iteration, so it kept the borrow
// live. That closure is sound for a binding that SURVIVES the iteration (a
// closure declared outside the loop, or an owner declared outside it), and
// wrong for a binding declared INSIDE the loop body — the next iteration
// re-creates it, so the reached use is a use of a DIFFERENT borrow.
//
// THE SPEC.
//
//   §28.3.9 (v0.359) *The consume-site freeze*: "A borrow is *live* at a
//   consume site when some path from the site reaches a later use of it —
//   K5.5's body-local backward dataflow, traced directly, through a `using
//   var` pin, or through a closure value that is itself still live."
//
// The freeze asks about a LATER USE OF THIS BORROW. On the next iteration the
// closure is a different value holding a different borrow (of the next
// iteration's owner), so the back edge reaches no later use of the borrow the
// consume site is about — the closure's live range is the ITERATION it was
// declared in.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. Row 1 asserts the
// program's own output, its exit code, and that LeakSanitizer counted
// nothing, so a silent zero-fill cannot pass; the program LOOPS (200
// iterations), so neither a leak of the per-iteration capture cell nor a
// per-iteration use-after-free can hide under the sanitizer's noise.
//
// The neighbouring shapes this fix must not weaken — a `move` BEFORE the
// closure's last use in the same iteration, a closure that ESCAPES the
// iteration (stored into an outer collection / outer field / outer local), and
// a closure that is live ACROSS iterations — are corpus candidates (named in
// the handback with their deciding spec sentences), because their VERDICT is
// what the corpus records. Row 2 is the suite's one boundary control: it is
// green on the base arm and RED under the plausible wrong fix that deletes the
// loop-back-edge leg of the liveness query (or narrows it by the holder
// alone) instead of narrowing it to the bindings the back edge actually
// reaches — the commit body names that wrong fix.

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

// The shared prelude: an ordinary class with one field.
constexpr const char* kPrelude =
    "module M; "
    "public class Res "
    "  public N: int := 7; "
    "  public constructor() end constructor; "
    "end Res; ";

// ROW 1 — the issue's literal repro. The closure `f` is declared INSIDE the
// loop body and its LAST use (`f()`) precedes the `move a` in the SAME
// iteration, so the borrow of `a` that `f` holds is dead at the consume site
// and the consume is legal.
//
// RED on the base: the freeze rejects the program (a compile error), so the
// program never runs and `r.ran` is false.
BEZEL_TEST(closure_per_iteration_borrow_8988,
           per_iteration_closure_move_after_its_last_use_is_admitted) {
    const std::string src =
        std::string(kPrelude) +
        "public class Sink "
        "  public static method Take(owned r: Res): int "
        "    var n := r.N; "
        "    r.Dispose(); "
        "    return n; "
        "  end Take; "
        "end Sink; "
        "public class Program "
        "  public static method Main(): int "
        "    var r := 0; "
        "    for var j in 0..<200 do "
        "      var a := new Res(); "
        "      var f := lambda(): int => a.N; "
        "      r := r + f(); "
        "      r := r + Sink.Take(move a); "
        "    end; "
        "    Console.WriteLine($\"{r}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "2800\n");
}

// ROW 2 — the suite's boundary control: the loop-back-edge leg of the
// liveness query must still fire for the bindings that SURVIVE the iteration.
//
// Here the OWNER `a` is declared OUTSIDE the loop body and the borrow holder
// `b` is a borrow alias declared INSIDE it. The alias's use (`b.N`) precedes
// the consume (the drop-on-overwrite `a := new Res()`) in the same iteration,
// so the textual "used-after" query finds nothing — but the next iteration
// re-executes `b.N` on the SAME owner, so the borrow IS live at the consume
// site and the freeze must reject it.
//
// This row is GREEN on the base (the freeze rejects it there too) and RED
// under the plausible wrong fix — deleting the loop-back-edge leg of the
// liveness query, or narrowing it by the HOLDER alone (which would skip the
// leg here because `b` is declared inside the body) — so it is the one
// boundary control the suite keeps.
BEZEL_TEST(closure_per_iteration_borrow_8988,
           borrow_holder_that_outlives_the_iteration_still_freezes_the_consume) {
    const std::string src =
        std::string(kPrelude) +
        "public class Program "
        "  public static method Main(): int "
        "    var r := 0; "
        "    var a := new Res(); "
        "    for var j in 0..<3 do "
        "      var b := a; "
        "      r := r + b.N; "
        "      a := new Res(); "
        "    end; "
        "    Console.WriteLine($\"{r}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    const JitResult r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "consume-site freeze"));
}

}  // namespace
