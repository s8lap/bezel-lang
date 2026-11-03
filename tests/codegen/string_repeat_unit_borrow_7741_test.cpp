// Fix #7741 — `String.Repeat(unit: string, count: int)` (stdlib §9.6) LEAKED
// the +1 producer local it was handed as its `unit` argument.
//
// THE DEFECT.
//
//   stdlib §9.6 (v0.353) declares, verbatim:
//
//     "public static pure method Repeat(unit: string, count: int): string;"
//
//   and spec §28.3.4 (v0.353) makes a PLAIN (no-modifier) parameter a BORROW
//   SOURCE — "Parameter `p` (no modifier) | Borrowed (source: caller)" — and
//   lists "a non-`owned` argument" among the BORROWING POSITIONS of an
//   owned-local read. So the `unit` argument is a BORROW: the callee copies
//   the unit's code points and keeps NO reference to it (`bz_str_repeat`,
//   src/runtime/bezel_runtime_strings.cpp, reads the unit's code points and
//   packs a fresh buffer), and the CALLER keeps the obligation to discharge
//   its own +1.
//
//   The compiler's conservative escape walk (StringEscapeWalker::walkCallArgs,
//   src/codegen/LlvmBackend_Setup.cpp) had NO relaxation for this callee. A
//   BUILT-IN callee has no resolved MethodSymbol, so the walk's #1430
//   plain-parameter relaxation never fired, and the `unit` argument fell
//   through to the walk's conservative ESCAPE. A +1 PRODUCER local bound to
//   the `unit` argument was therefore EXCLUDED from the scope-end release
//   set: its own +1 was never discharged, and it leaked.
//
// MEASURED on the base tree (this lane's own worktree base `c3c33264`, the
// issue's own repro, 200 iterations, `scripts/agent_check.sh --probe --asan`):
//
//     1600
//     ==================================================================
//     ==NNNN==ERROR: LeakSanitizer: detected memory leaks
//     Direct leak of 6400 byte(s) in 200 object(s) allocated from:
//     Indirect leak of 1000 byte(s) in 200 object(s) allocated from:
//     SUMMARY: AddressSanitizer: 7400 byte(s) leaked in 400 allocation(s).
//
//   7400 B in 400 allocations = 200 iterations x (a 32-byte `bz.string` header
//   + its 5-byte data buffer) — the +1 producer `"ab" + "cd"` the local `w`
//   held, once per iteration, never released.
//
// THE CONTROLS (the issue's own): the same producer bound to a local and
// consumed as `w + w` is CLEAN, and a LITERAL local (`var w := "abc"`) handed
// to `String.Repeat(w, 2)` is CLEAN (a literal is the immortal sentinel, so
// its retain/release are no-ops). The defect needs a PRODUCER-BOUND local
// passed as the `unit` argument.
//
// WHAT THIS FILE PINS.
//
//   * The LEAK oracle: the LSan SUMMARY's own NUMBERS (allocations / bytes),
//     parsed out of the sanitizer report the AOT harness records. A row that
//     only asserted the printed value would NOT observe a leak; a row that
//     asserts the leak-size NUMBER observes it exactly — 0 on the fixed tree,
//     400 allocations / 7400 bytes on the pre-fix tree.
//   * The VALUE oracle: the printed length (the issue's own observable), so
//     a fix that freed the local too early (a use-after-free) or dropped the
//     repetition is caught too.
//   * The DOUBLE-FREE guard: the INLINE-producer spelling
//     (`String.Repeat("ab" + "cd", 2)`) hands the callee a +1 the CALLER owns,
//     and the callee's own arm releases it (the #7129 arm). The relaxation
//     added here must not make that arm release it a SECOND time.
//
// FAIL-WITHOUT (CLAUDE.md: "Pin THE bug" / "Prove fail-without mechanically").
// The source files were reverted with `git checkout --` (never `git stash` —
// the refs/stash is shared across worktrees), the TEST BINARY rebuilt
// (`scripts/agent_check.sh` rebuilds it; rebuilding only `bezelc` would test
// the old compiler), and the rows re-run. The numbers are in the lane's final
// report.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// THE ORACLE. `leakedAllocations` is the COUNT of allocations LeakSanitizer
// reports as leaked: 0 when there is no SUMMARY line (LSan prints one only
// when it finds leaks), and the summary's own number when there is one.
//
// ⚠ THIS IS THE OBSERVATION, NOT A TAUTOLOGY. The report is NOT reachable
// from `r.diagnostics.hasErrors()` alone — it is the harness's Error
// diagnostic's MESSAGE, and this helper reads that message. A row that only
// asserted the printed value would not observe the leak at all.
//
// The row also asserts that the program ran to completion with the expected
// output, that the exit code is 0, and that NO sanitizer report of any kind
// is present — so a double free (which ASan reports as "attempting
// double-free") fails the row too.
void expectNoLeak(const JitResult& r, const char* out) {
    // The row must NOT have skipped: a skipped row asserts nothing.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary = bezel::test::aot_detail::parseLeakSummary(
        diagnosticText(r), leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    // The COUNT, printed so a failing row's log carries the NUMBER the leak
    // oracle measured (the pre-fix compiler reads 400 allocations / 7400
    // bytes here).
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
    // No sanitizer report of any kind: a double free would land here.
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") ==
                      std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

}  // namespace

// ===========================================================================
// 1/6 — THE ISSUE'S OWN REPRO.
//
// `var w := "ab" + "cd";` binds a +1 PRODUCER to a local; `String.Repeat(w, 2)`
// passes it as the `unit` argument — a PLAIN (borrow) parameter.
//
// PRE-FIX (measured): the program prints "1600" and LSan reports
// "7400 byte(s) leaked in 400 allocation(s)".
// POST-FIX: the program prints "1600" and LSan reports NOTHING.
//
// The 200 iterations are the issue's own count: under LSan's default options
// a one-shot repro is vacuous (CLAUDE.md: "A LEAK REPRO MUST BE LOOPED
// (50-500x)").
// ===========================================================================
BEZEL_TEST(string_repeat_unit_borrow_7741,
           producer_local_passed_as_unit_leaks_spec_28_3_4) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class C
  public static method Main(): int
    var i := 0;
    var n := 0;
    while i < 200 do
      var w := "ab" + "cd";
      n := n + String.Repeat(w, 2).Length;
      i := i + 1;
    end;
    Console.WriteLine(n);
    return 0;
  end Main;
end C;
)BZL"),
                 "1600\n");
}

// ===========================================================================
// 2/6 — THE SAME SHAPE IN THE NAMED SPELLING (§22.5).
//
// `String.Repeat(unit := w, count := 2)` names the parameters it binds. The
// walk's other string relaxations are gated on an UNNAMED argument (their
// parameter mapping is positional), so the named spelling needs the
// recognizer to resolve the parameter the argument NAMES — the two-way
// resolution `fnObjSiteSlot` (src/codegen/LlvmBackend_Layout.cpp) uses for
// the collection-shaped sites.
//
// PRE-FIX (measured): the same 7400 B / 400 allocations leak.
// POST-FIX: "1600", no leak.
// ===========================================================================
BEZEL_TEST(string_repeat_unit_borrow_7741,
           producer_local_passed_as_named_unit_leaks_spec_22_5) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class C
  public static method Main(): int
    var i := 0;
    var n := 0;
    while i < 200 do
      var w := "ab" + "cd";
      n := n + String.Repeat(unit := w, count := 2).Length;
      i := i + 1;
    end;
    Console.WriteLine(n);
    return 0;
  end Main;
end C;
)BZL"),
                 "1600\n");
}

// ===========================================================================
// 3/6 — THE CONTROL: THE INLINE PRODUCER.
//
// `String.Repeat("ab" + "cd", 2)` hands the callee a +1 the CALLER owns, and
// the callee's own arm (the #7129 arm, src/codegen/LlvmBackend_Calls2.cpp)
// releases it after the runtime helper has copied the code points. The
// relaxation added for the local shape must NOT make that arm release it a
// SECOND time — a double free would be an ASan "attempting double-free"
// abort, which this row observes.
//
// PRE-FIX and POST-FIX: "1600", no leak, no double free.
// ===========================================================================
BEZEL_TEST(string_repeat_unit_borrow_7741,
           inline_producer_unit_is_released_exactly_once_spec_28_9) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class C
  public static method Main(): int
    var i := 0;
    var n := 0;
    while i < 200 do
      n := n + String.Repeat("ab" + "cd", 2).Length;
      i := i + 1;
    end;
    Console.WriteLine(n);
    return 0;
  end Main;
end C;
)BZL"),
                 "1600\n");
}

// ===========================================================================
// 4/6 — THE CONTROL: A LITERAL UNIT.
//
// The issue's own control: `var w := "abc"; String.Repeat(w, 2)` is clean on
// the base tree (a literal is the immortal sentinel, so its retain and its
// release are no-ops) — this row pins that the relaxation did not change it.
//
// PRE-FIX and POST-FIX: "1200", no leak.
// ===========================================================================
BEZEL_TEST(string_repeat_unit_borrow_7741,
           literal_unit_local_stays_clean_spec_28_9) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class C
  public static method Main(): int
    var i := 0;
    var n := 0;
    while i < 200 do
      var w := "abc";
      n := n + String.Repeat(w, 2).Length;
      i := i + 1;
    end;
    Console.WriteLine(n);
    return 0;
  end Main;
end C;
)BZL"),
                 "1200\n");
}

// ===========================================================================
// 5/6 — THE LOCAL IS STILL ALIVE AFTER THE CALL.
//
// The relaxation arms the local's SCOPE-END release; it must not free the
// value AT the call. This row reads the local's own length AFTER the call, so
// a fix that freed it early would be a heap-use-after-free (ASan) and a fix
// that freed it at the wrong time would print the wrong number.
//
// PRE-FIX: "2400" (the local is intact — it simply leaks at scope end).
// POST-FIX: "2400", no leak.
// ===========================================================================
BEZEL_TEST(string_repeat_unit_borrow_7741,
           producer_local_is_live_after_the_call_spec_28_3_4) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class C
  public static method Main(): int
    var i := 0;
    var n := 0;
    while i < 200 do
      var w := "ab" + "cd";
      n := n + String.Repeat(w, 2).Length;
      n := n + w.Length;
      i := i + 1;
    end;
    Console.WriteLine(n);
    return 0;
  end Main;
end C;
)BZL"),
                 "2400\n");
}

// ===========================================================================
// 6/6 — THE OTHER SPELLING OF THE HEAD: `string.Repeat`.
//
// stdlib §9.6's head is `String`, and `string` is its alias (the keyword).
// The recognizer reads the qualifier's NAME, so both spellings must reach the
// same relaxation — a spelling that leaked while the other did not would be
// the same defect wearing a different hat.
//
// PRE-FIX (measured): the same 7400 B / 400 allocations leak.
// POST-FIX: "1600", no leak.
// ===========================================================================
BEZEL_TEST(string_repeat_unit_borrow_7741,
           lowercase_string_head_spelling_leaks_spec_28_3_4) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class C
  public static method Main(): int
    var i := 0;
    var n := 0;
    while i < 200 do
      var w := "ab" + "cd";
      n := n + string.Repeat(w, 2).Length;
      i := i + 1;
    end;
    Console.WriteLine(n);
    return 0;
  end Main;
end C;
)BZL"),
                 "1600\n");
}
