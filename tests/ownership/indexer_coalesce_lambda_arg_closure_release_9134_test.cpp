// Regression test for issue #9134 — a lambda written AT the index position of
// an indexer `??=` assignment (`fl[lambda() => 7] ??= 3;`) leaked its closure:
// the `??=` path is a SEPARATE lowering (`lowerAccessorNullCoalescingAssign`),
// it PINS the index expression, and the pin makes the shared index-argument
// discharge decline it.
//
// THE DEFECT. §6.6 (v0.359) evaluates the LHS chain of a `??=` ONCE ("every
// constituent of the left-hand side's lvalue chain ... is evaluated exactly
// once, left to right: ... and every index expression in the chain"), so
// `lowerAccessorNullCoalescingAssign` PINS the index expression (#2929's
// `exprValueOverride_` pin) and the getter and the setter share ONE lowered
// index value.
//
// `releaseIndexArgClosureProducers` — the caller's drop of an index argument's
// +1 closure producer — DECLINES a PINNED index expression: §22.3's
// copy-in/copy-out bracket exclusion (a `ref`/`out` bracket owns the index's
// lifetime across the getter and the setter halves, so releasing it there
// would double-free it). The `??=` path's pin is §6.6's once-only pin, not
// §22.3's bracket, but the exclusion cannot tell them apart — so the
// getter's discharge (#9083's) and the setter's (#4078's) BOTH declined the
// producer, and its +1 was stranded.
//
// MEASURED on r30-c1 @ 4e5dbd3fe, `bezelc --sanitize=address --link=system
// --checked-disposal`, the issue's own 200-iteration repro:
//
//   Direct leak of 12736 byte(s) in 199 object(s) allocated from:
//       #0 ... in calloc ...
//       #1 ... in main ...
//   SUMMARY: AddressSanitizer: 12736 byte(s) leaked in 199 allocation(s).
//
// 64 bytes per call — the closure object alone (the probe's lambda captures
// nothing, so there is no capture cell and no cell payload).
//
// THE SPEC. §28.14 (v0.359), *The call-bounded default*: "A function-typed
// parameter declared *without* `scoped` is **call-bounded** … the callee
// receives the callback for the duration of the call only". §23.5 (v0.359):
// "`ref` and `out` parameters are not allowed in indexer parameter lists" — so
// an indexer's function-typed parameter is plain, hence call-bounded, and the
// accessor never consumes the callback: the producer's +1 is the CALLER's to
// drop once the accessor has returned. §28.9 (v0.359): the closure object is
// runtime-managed, so its own reference must be dropped exactly once. §6.6
// (v0.359): the LHS chain — every index expression in it — is evaluated
// exactly once.
//
// THE CHANGE. `lowerAccessorNullCoalescingAssign` now discharges the index
// argument's closure producer at the JOIN — after the getter (unconditional)
// and the setter (the assign branch) have both consumed the index. The
// discharge is ONCE: the getter's and the setter's own discharges both
// decline the pinned index, so the join is the only place the producer's +1
// is dropped.
//
// The `!diverged_` gate is the discipline every sibling discharge uses
// (#429 / #9068 / #4078 / #9110): the release is not emitted into unreachable
// IR after an accessor that threw; the unwind edge's caller owns the throw
// path (a leak there is the safe direction — the indexer's THROW path is
// #9135, deferred).
//
// THE ORACLE. Every row runs the program TWICE: once plain (`runAot`) for the
// VALUE oracle — the program's own exit code and output — and once checked
// (`runAotChecked`) for the MEMORY oracle. These programs must leak NOTHING
// at all: MEASURED, the `??=` path leaks no receiver copy (the plain SETTER
// path does — a separate, pre-existing 16-byte receiver-copy defect this
// issue does not own), so the closure's 64 bytes per call is the WHOLE leak.
//
// ROWS.
//   Row 1 — the issue's own program, VERBATIM: `fl[lambda() => 7] ??= 3;` over
//           200 iterations, on a nullable-valued indexer whose value starts
//           null (so the setter runs). RED on the base (12736 B / 199
//           allocations), GREEN with the fix.
//   Row 2 — the GETTER-ONLY twin: the same `??=` on an indexer whose value is
//           ALREADY non-null, so the setter does NOT run and the getter is
//           the index's only consumer. RED on the base (the same leak), GREEN
//           with the fix. It proves the discharge does not depend on the
//           setter running.
//   Row 3 — GREEN CONTROL: the `??=` with a CALLER-OWNED closure LOCAL as the
//           index (`var g := lambda() => 7; fl[g] ??= 3;`), the path #9102
//           fixed. CLEAN on both arms. It is the OVER-BROAD guard: a
//           predicate that matched a local would release the local's +1 a
//           second time, and the local's own scope-end release would free it
//           again (a double free under ASan).
//   Row 4 — GREEN CONTROL: the PLAIN getter with a lambda literal
//           (`fl[lambda() => 7];`), the path #9083 fixed. CLEAN on both arms.
//           It is the DOUBLE-RELEASE guard for the shared discharge helper:
//           the `??=` path's getter and the plain path's getter use the same
//           one, and a fix that released the producer twice turns this row
//           red with a bad free / use-after-free under ASan.
//
// per §28.14, §28.9, §23.4, §23.5, §6.6 (issue #9134)

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

// The VALUE oracle: the program compiled, ran to completion, exited with its
// own exit code and printed exactly this output. The plain (uninstrumented)
// run has no sanitizer, so its exit code is the program's own.
void expectValue(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// The MEMORY oracle: the checked run leaked NOTHING at all, and no memory
// error fired.
//
// MEASURED, the `??=` path leaks no receiver copy (the plain SETTER path
// does — a separate, pre-existing 16-byte receiver-copy defect this issue
// does not own), so a `??=`-only program has no residual: the closure's
// 12736 B / 199 allocations over the issue's own 200-iteration repro is the
// WHOLE leak.
void expectNoLeak(const JitResult& r) {
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

// The indexer: a nullable-valued read-write indexer over a plain
// function-typed index parameter. `init` is the value the field starts with —
// null makes the `??=` STORE (the setter runs), a non-null value makes it a
// no-op (the getter is the index's only consumer).
std::string coalesceModule(const std::string& cls, const std::string& init,
                            const std::string& stmt) {
    return "module " + cls + "; "
           "public class FlN" + cls + " "
           "  private _v: int? := " + init + "; "
           "  public property [q: () -> int]: int? "
           "    get => _v; "
           "    set => do "
           "      _v := value; "
           "    end; "
           "  end; "
           "  public constructor() end constructor; "
           "end FlN" + cls + "; "
           "public class Program" + cls + " "
           "  public static method Main(): int "
           "    for var j in 0..<200 do "
           "      using var fl := new FlN" + cls + "(); "
           "      " + stmt + " "
           "    end; "
           "    Console.WriteLine(0); "
           "    return 0; "
           "  end Main; "
           "end Program" + cls + ";";
}

// ── Row 1 (§28.14 / §28.9 / §23.5 / §6.6): the issue's own program, VERBATIM.
//
// `fl[lambda() => 7] ??= 3;` — the index argument is a lambda literal, a +1
// closure producer the CALLER owns. The indexer receives it as a call-bounded
// callback and keeps no reference to it, so the caller must drop it once the
// accessor has returned.
//
// The value starts null, so the `??=` STORES: the getter and the setter both
// consumed the index, and the join discharges it once.
//
// RED on the base (12736 B / 199 allocations), GREEN with the fix.
BEZEL_TEST(indexer_coalesce_lambda_arg_closure_release_9134,
           coalesce_assignment_releases_the_index_argument_closure) {
    const std::string src =
        coalesceModule("C9134", "null", "fl[lambda() => 7] ??= 3;");
    auto v = runAot(src);
    expectValue(v, 0, "0\n");
    auto r = runAotChecked(src);
    expectNoLeak(r);
}

// ── Row 2 (§28.14 / §28.9 / §6.6): the GETTER-ONLY twin.
//
// The same `??=` on an indexer whose value is ALREADY non-null: the setter
// does NOT run, so the getter is the index's only consumer. The discharge is
// still owed — the getter consumed the callback — and it is still ONE, at the
// join.
//
// RED on the base (the same leak), GREEN with the fix.
BEZEL_TEST(indexer_coalesce_lambda_arg_closure_release_9134,
           coalesce_assignment_with_a_present_value_releases_the_index_argument_closure) {
    const std::string src =
        coalesceModule("P9134", "5", "fl[lambda() => 7] ??= 3;");
    auto v = runAot(src);
    expectValue(v, 0, "0\n");
    auto r = runAotChecked(src);
    expectNoLeak(r);
}

// ── Row 3 (GREEN CONTROL — the CALLER-OWNED closure LOCAL as the index): the
// path #9102 fixed. CLEAN on the base and clean with the fix.
//
// It is the OVER-BROAD guard: a predicate that matched a local would release
// the local's +1 a second time, and the local's own scope-end release would
// free it again (a double free under ASan).
BEZEL_TEST(indexer_coalesce_lambda_arg_closure_release_9134,
           coalesce_assignment_with_a_caller_owned_closure_local_stays_clean) {
    const std::string src = coalesceModule(
        "L9134", "null",
        "var g := lambda() => 7; fl[g] ??= 3;");
    auto v = runAot(src);
    expectValue(v, 0, "0\n");
    auto r = runAotChecked(src);
    expectNoLeak(r);
}

// ── Row 4 (GREEN CONTROL — the PLAIN getter with a lambda literal): the path
// #9083 fixed. CLEAN on the base and clean with the fix.
//
// It is the DOUBLE-RELEASE guard for the shared discharge helper: the `??=`
// path's getter and the plain path's getter use the same one, and a fix that
// released the producer twice turns this row red with a bad free /
// use-after-free under ASan.
BEZEL_TEST(indexer_coalesce_lambda_arg_closure_release_9134,
           plain_getter_with_a_lambda_literal_stays_clean) {
    const std::string src =
        "module G9134; "
        "public class FlN9134G "
        "  private _v: int? := null; "
        "  public property [q: () -> int]: int? "
        "    get => _v; "
        "    set => do "
        "      _v := value; "
        "    end; "
        "  end; "
        "  public constructor() end constructor; "
        "end FlN9134G; "
        "public class Program9134G "
        "  public static method Main(): int "
        "    for var j in 0..<200 do "
        "      using var fl := new FlN9134G(); "
        "      var x := fl[lambda() => 7]; "
        "    end; "
        "    Console.WriteLine(0); "
        "    return 0; "
        "  end Main; "
        "end Program9134G;";
    auto v = runAot(src);
    expectValue(v, 0, "0\n");
    auto r = runAotChecked(src);
    expectNoLeak(r);
}

}  // namespace
