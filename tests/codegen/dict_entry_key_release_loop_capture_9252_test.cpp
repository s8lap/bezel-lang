// Codegen — fix #9252 (§28.11 *Collection element ownership* / §28.9
// *Runtime-managed types* / §27.5 *Closures*, v0.359): a string-keyed
// `DictionaryEntry<string,V>` local bound to `d.First()` and captured by a
// NON-ESCAPING closure BEHIND AN `if` INSIDE A LOOP leaked its rebuilt Key —
// the one shape #4165's fix does not reach.
//
// THE DEFECT. §28.11's element slot policy gives the copy-out of a
// `DictionaryEntry<string,V>`'s Key the runtime-managed-reference treatment:
// "copy out (a runtime-managed reference the copy holds — a
// `DictionaryEntry<string, V>`'s `Key` — keeps §28.9's unbounded lifetime
// through every later mutation or disposal of the collection)". The Key the
// collection hands out is a FRESH string (a new `bz.string` header plus a
// new payload, refcount 1 — `bz_string_born`), so the local holds ONE +1 on
// it and the compiler must discharge that +1 exactly once.
//
// For a captured local the ordinary scope-end Key release is DECLINED (the
// `StringEscapeWalker` treats ANY closure capture as an escape for that
// predicate's purposes — #3720's documented trade-off), so the +1 is left to
// the capture cell's own free: #4165 put a `dictEntryKeyReleaseDeferred` flag
// on the cell free, and the free emits the GEP of the cell's field 0 plus
// `bz_string_release` before freeing the cell.
//
// #4165 registered that flag in TWO places: `lowerLambda`'s #3508 arm (the
// owner-scope deferral) and `consumeLastLambdaCapturedCells`'s main arm. A
// THIRD place registers the same owner-scope deferred cell free — the #969 /
// #4210 arm of `consumeLastLambdaCapturedCells` — and it did NOT carry the
// flag. That arm is the one that registers the deferred free exactly when
// #3508's arm DECLINED, and #3508 declines when a LOOP is in scope (a loop
// allocates a cell per iteration, so deferring to the owner scope would free
// only the last iteration's cell). So for a capture behind an `if` inside a
// loop, the cell was freed and the Key's +1 was never discharged.
//
// MEASURED on r30-c1 @ d66898662 (the issue's own repro, verbatim, under
// ASan/LSan):
//
//   2200
//   Direct leak of 6400 byte(s) in 200 object(s) allocated from:  #0 calloc
//   Indirect leak of 600 byte(s) in 200 object(s) allocated from: #0 calloc
//   SUMMARY: AddressSanitizer: 7000 byte(s) leaked in 400 allocation(s).
//
// 32 B (the `bz.string` header) + 3 B (the "xy" payload) per iteration. The
// program's own value is CORRECT (2200 = 200 × (2 + 9)) — a pure leak, never
// a use-after-free or a double-free.
//
// THE IR, before the fix (`BEZEL_DUMP_IR=1`): the cell is allocated per
// iteration (`%105 = calloc(...)`, the DictionaryEntry struct copied into it
// by value), the rebuilt Key (`%dk.key`, a fresh `bz.string` with refcount 1)
// is stored in the cell's field 0, and the cell is freed at the owner scope
// (`%cell.deferred.ptr31 = load ptr, ptr %5` / `call void @free(...)`) — with
// NO `bz_string_release` for the Key anywhere in the module.
//
// THE FIX. Carry #4165's flag onto the deferred cell free that the #969 /
// #4210 arm registers. The safety case is #4165's, unchanged: the deferred
// free is registered only for a cell of a `cellFreeEligible_` local (a
// non-escaping capture), so `singleCaptureLocals_` holds — exactly one
// holder, exactly one +1 to balance — and the local's own scope-end Key
// release is still declined, so the cell free's release is the only one.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. The row asserts
// the program's own output and exit value as well as the leak count, so a
// silent zero-fill cannot pass, and it LOOPS (200 iterations inside the
// program, the issue's own repro), so the 35 B/iteration leak cannot hide
// under the sanitizer's noise.
//
// FAIL-WITHOUT / PASS-WITH. The three rows are RED on the base (r30-c1 @
// d66898662): the two leak rows report "SUMMARY: AddressSanitizer: 7000
// byte(s) leaked in 400 allocation(s)" (identical bytes — the `d.First()` and
// the `d.Last()` terminals rebuild the Key the same way), and the third row
// SEGFAULTS (exit 139 — the Key release loading through the null cell on the
// path where the capture site never ran). All three are GREEN with the fix.

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

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor the runtime's §28.9 over-release check
// fired. `runAotChecked` reports an abort and any sanitizer report as
// `ran == false`.
void didNotCrash(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("refcount over-release") == std::string::npos);
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes,
                                                   leakedAllocs);
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

// The issue's own repro, VERBATIM: a string-keyed DictionaryEntry local bound
// to `d.First()`, captured by a non-escaping closure BEHIND AN `if` INSIDE A
// LOOP. The loop runs 200 times, so the leak is 200 × 35 B.
//
// The program's own value: "xy".Length (2) + the value (9) = 11 per
// iteration, 200 iterations → 2200.
std::string issueRepro() {
    return
        "module M; "
        "import System; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var d := new Dictionary<string, int>(); "
        "      d.Add(\"xy\", 9); "
        "      var fe := d.First(); "
        "      if i >= 0 then "
        "        var g := lambda() => fe.Key.Length + fe.Value; "
        "        t := t + g(); "
        "      end if; "
        "    end for; "
        "    Console.WriteLine($\"{t}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
}

// The same shape through a DIFFERENT terminal: `d.Last()` reaches the same
// `isFreshDictEntryKeyReleaseTerminalLocal` eligibility gate as `d.First()`
// (per #3720 the Key is rebuilt identically by both), so it must be released
// at the same point — a fix that keyed on the terminal rather than on the
// mechanism would leave this row RED.
std::string lastTerminalRepro() {
    return
        "module M; "
        "import System; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var d := new Dictionary<string, int>(); "
        "      d.Add(\"xy\", 9); "
        "      var fe := d.Last(); "
        "      if i >= 0 then "
        "        var g := lambda() => fe.Key.Length + fe.Value; "
        "        t := t + g(); "
        "      end if; "
        "    end for; "
        "    Console.WriteLine($\"{t}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
}

// The null-cell CRASH row: the capture site behind an `if` WHOSE BRANCH IS
// NEVER TAKEN. The capture cell is never formed on this path, so the deferred
// cell free finds a NULL holder — and the Key release loads THROUGH the cell.
//
// On the base (r30-c1 @ d66898662) this SEGFAULTS — a real invalid read, not
// a sanitizer artefact:
//   "Segmentation fault (core dumped)" (exit 139) — the
//   `dictek.deferred.val` load through the null cell. With the fix the
//   program runs to completion.
//
// The row runs on the PLAIN `runAot` oracle: the crash is a genuine
// segmentation fault, so the unchecked path catches it too, and the plain
// oracle is what lets the row assert the program's own exit code (the
// checked oracle would report the 35 B of #9348's residual leak as a
// non-zero exit and the row could not distinguish the crash from that leak).
//
// The 35 B the rebuilt Key still leaks on this path (the local's own +1 has
// no discharge point when the capture site never ran) is #9348's scope; this
// row pins the CRASH, not that leak.
std::string neverTakenBranchRepro() {
    return
        "module M; "
        "import System; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"xy\", 9); "
        "    var fe := d.First(); "
        "    if t > 100 then "
        "      var g := lambda() => fe.Key.Length + fe.Value; "
        "        t := t + g(); "
        "    end if; "
        "    Console.WriteLine($\"{t}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
}

}  // namespace

// ---------------------------------------------------------------------------
// THE REGRESSION ROW — the issue's own repro, verbatim.
// ---------------------------------------------------------------------------
//
// RED on the base (r30-c1 @ d66898662): "SUMMARY: AddressSanitizer: 7000
// byte(s) leaked in 400 allocation(s)" — 35 B per iteration (the 32 B
// `bz.string` header + the 3 B "xy" payload).
// GREEN with the fix: clean.
BEZEL_TEST(dict_entry_key_release_loop_capture_9252,
           first_captured_behind_if_in_loop_leak_free) {
    JitResult r = runAotChecked(issueRepro());
    expectClean(r, 0, "2200\n");
}

// The `d.Last()` sibling of the row above: the same mechanism through the
// other terminal.
//
// RED on the base (r30-c1 @ d66898662): "SUMMARY: AddressSanitizer: 7000
// byte(s) leaked in 400 allocation(s)" — identical bytes.
// GREEN with the fix: clean.
BEZEL_TEST(dict_entry_key_release_loop_capture_9252,
           last_captured_behind_if_in_loop_leak_free) {
    JitResult r = runAotChecked(lastTerminalRepro());
    expectClean(r, 0, "2200\n");
}

// The null-cell CRASH row: the capture site's branch is never taken, so the
// deferred cell free finds a null holder.
//
// RED on the base (r30-c1 @ d66898662): the program SEGFAULTS —
// "Segmentation fault (core dumped)", exit 139 — on the `dictek.deferred.val`
// load through the null cell.
// GREEN with the fix: the program runs, prints 0 and exits 0.
BEZEL_TEST(dict_entry_key_release_loop_capture_9252,
           capture_behind_never_taken_branch_does_not_crash) {
    JitResult r = runAot(neverTakenBranchRepro());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "0\n");
}
