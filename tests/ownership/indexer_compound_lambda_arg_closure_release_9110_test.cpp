// Regression test for issue #9110 — a lambda written AT the index position of
// an indexer COMPOUND assignment (`fl[lambda() => 7] += 3;`) leaked its
// closure: the compound path lowers the index ONCE, shares it between the
// getter and the setter, and discharged neither the STRING producer nor the
// CLOSURE one.
//
// THE DEFECT. `Emitter::lowerIndexerCompoundCore` — the compound-assignment
// half of §6.6 / §17.6's indexer desugaring — lowers the index argument once
// and passes the same value to BOTH the getter and the setter call. The
// STRING producer release (`releaseIndexArgStringProducers`, #2717) was
// wired there; the CLOSURE one (`releaseIndexArgClosureProducers`) was not,
// and the compound path does not go through `lowerIndexerGet` either — so
// the producer's +1 was minted and never dropped, in the imported path and
// in the single-module one alike.
//
// MEASURED on r30-c1 @ 116823e00, `bezelc --sanitize=address`, 200 iterations:
//
//   Direct leak of 12800 byte(s) in 200 object(s) allocated from:
//       #1 ... in main ...
//
// 64 bytes per call — the closure object alone (the probe's lambda captures
// nothing, so there is no capture cell and no cell payload).
//
// THE SPEC. §28.14 (v0.359), *The call-bounded default*: "A function-typed
// parameter declared *without* `scoped` is **call-bounded** … the callee
// receives the callback for the duration of the call only". §23.5 (v0.359):
// "`ref` and `out` parameters are not allowed in indexer parameter lists" —
// so an indexer's function-typed parameter is plain, hence call-bounded, and
// the accessor never consumes the callback: the producer's +1 is the
// CALLER's to drop once the accessor has returned. §28.9 (v0.359): the
// closure object is runtime-managed, so its own reference must be dropped
// exactly once.
//
// THE CHANGE. `lowerIndexerCompoundCore` now discharges the index argument's
// closure producer on the normal-return edge of each setter path that already
// discharges its STRING producer — the imported arm, the class-vtable arm and
// the inline-setter arm:
//
//     if (!diverged_) releaseIndexArgClosureProducers(*ix, ps, idxArgVals);
//
// The `!diverged_` gate is the plain setter's own discipline (#4078's setter
// twin) and #9068's vtable sibling: the release is not emitted into
// unreachable IR after a setter that threw; the unwind edge's caller owns
// the throw path (a leak there is the safe direction).
//
// THE ORACLE. Every row runs the program TWICE: once plain (`runAot` /
// `runAotModules`) for the VALUE oracle — the program's own exit code and
// output — and once checked (`runAotChecked` / `runAotModulesChecked`) for
// the MEMORY oracle.
//
// THE MEMORY ORACLE WAS A FLOOR, and the reason was a SEPARATE, PRE-EXISTING
// defect: an indexer SETTER leaked what this file's header called "a COPY of
// the receiver" (16 bytes per call — the receiver object's size), on the base
// and on every arm, imported and single-module alike. That leak was NOT this
// issue's and was NOT touched by this fix, but it was in the same program, so
// the rows could not assert "no leak at all". They asserted instead that what
// remained did not EXCEED the receiver's own residual: the closure's 64 bytes
// per call must be GONE. On the base the same programs leaked 16000 byte(s)
// in 400 allocation(s) — the receiver's 3200 B / 200 plus the closure's
// 12800 B / 200 — so the rows were RED on the base.
//
// fix #9156 (§28.2 / §28.3, v0.359): THE FLOOR IS GONE. The "receiver copy"
// this floor tolerated was never a copy: it was the RECEIVER'S OWN BLOCK,
// quarantined instead of reclaimed (§28.2's checked-mode latency), because
// the index receiver of an indexer ASSIGNMENT was wrongly read as
// "transferred" by the checked-disposal free-eligibility walk. Fix #9156
// reclaims it, so the three floors below are TIGHTENED to "no leak at all"
// (a strengthening). The row NAMES are kept as they are — issue #9156 and
// its lane brief refer to row 3 by name.
//
// ⇒ The rows are now GREEN on the current base for the memory leg as well:
// the programs leak NOTHING (the closure's 64 B per call was released by
// #9110, and the receiver's 16 B per call by #9156). They stay RED on the
// pre-#9110 base (16000 B / 400 allocations) and RED on the pre-#9156 base
// for the receiver's residual (3200 B / 200 allocations, in
// CHECKED-DISPOSAL mode).
//
// ROWS.
//   Row 1 — the issue's own shape: an IMPORTED indexer, a compound
//           assignment, 200 iterations. RED on the base (16000 B / 400
//           allocations), GREEN with the fix (3200 B / 200 — the receiver's
//           residual only), and — after fix #9156 — leak-free.
//   Row 2 — the SINGLE-MODULE twin (the compound path's inline-setter arm).
//           RED on the base, GREEN with the fix, leak-free after #9156.
//   Row 3 — GREEN CONTROL: the imported PLAIN setter (`fl[lambda() => 7]
//           := 3;`), whose closure producer #4078's setter twin already
//           discharged. Green on the base and green with the fix; it is the
//           DOUBLE-RELEASE guard — a fix that released the producer twice
//           turns this row red with a bad free / use-after-free under ASan.
//           fix #9156 TIGHTENED this row from "at most the receiver's
//           residual (3200 B / 200)" to "no leak at all"; the row NAME is
//           kept (issue #9156 refers to it by name).
//   Row 4 — GREEN CONTROL: the imported PLAIN getter (`fl[lambda() => 1];`),
//           the path #9083 fixed. CLEAN on both arms (no leak at all): it is
//           the guard that the compound fix does not release the getter's
//           producer a second time.
//
// per §28.14, §28.9, §23.4, §23.5, §6.6, §17.6 (issue #9110)

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotModules;
using bezel::test::runAotModulesChecked;

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

// fix #9156 (§28.2 / §28.3, v0.359): the receiver-copy residual this issue
// did not own is GONE (the receiver's own block is reclaimed now, not
// quarantined), so the three rows that used the floor (`expectLeakAtMost`,
// removed) are TIGHTENED to `expectNoLeak` — a strengthening, named in the
// commit body.

// The MEMORY oracle for a program that must leak NOTHING at all.
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

// The imported module's read-write indexer — a getter and a setter over a
// plain function-typed index parameter. The getter returns `_last + q()` and
// the setter stores `q() + value`, so the compound assignment's arithmetic
// is observable in the program's output.
std::string libModule(const std::string& className) {
    return "module " + className + "; "
           "public class FlS" + className + " "
           "  private _last: int := 0; "
           "  public property [q: () -> int]: int "
           "    get => _last + q(); "
           "    set => do _last := q() + value; end; "
           "  end; "
           "  public constructor() end constructor; "
           "end FlS" + className + ";";
}

// The importing module's program: the compound assignment over the imported
// indexer, then a plain read. 200 iterations, so the per-call leak (64 B in
// 1 allocation) is unambiguous.
//
// The arithmetic: the getter returns `_last + q()` = 0 + 7 = 7; `+= 3` makes
// it 10; the setter stores `q() + value` = 7 + 10 = 17; the plain read then
// returns `_last + q()` = 17 + 1 = 18. So 200 * 18 = 3600.
std::string appModule(const std::string& className, const std::string& stmt) {
    return "module App" + className + "; "
           "import " + className + "; "
           "public class Program" + className + " "
           "  public static method Main(): int "
           "    var t := 0; "
           "    for var j in 0..<200 do "
           "      using var fl := new " + className + ".FlS" + className + "(); "
           "      " + stmt + " "
           "      t := t + fl[lambda() => 1]; "
           "    end; "
           "    Console.WriteLine(t); "
           "    return 0; "
           "  end Main; "
           "end Program" + className + ";";
}

// The single-module twin of the imported program: the same read-write
// indexer, declared in the program's own module.
std::string singleModule(const std::string& className,
                         const std::string& stmt) {
    return "module " + className + "; "
           "public class FlS" + className + " "
           "  private _last: int := 0; "
           "  public property [q: () -> int]: int "
           "    get => _last + q(); "
           "    set => do _last := q() + value; end; "
           "  end; "
           "  public constructor() end constructor; "
           "end FlS" + className + "; "
           "public class Program" + className + " "
           "  public static method Main(): int "
           "    var t := 0; "
           "    for var j in 0..<200 do "
           "      using var fl := new FlS" + className + "(); "
           "      " + stmt + " "
           "      t := t + fl[lambda() => 1]; "
           "    end; "
           "    Console.WriteLine(t); "
           "    return 0; "
           "  end Main; "
           "end Program" + className + ";";
}

// ── Row 1 (§28.14 / §28.9 / §23.5): the issue's own shape — an IMPORTED
// indexer, a COMPOUND assignment whose index argument is a lambda literal.
// 200 iterations.
//
// RED on the base (16000 B / 400 allocations: the receiver's 3200 / 200 plus
// the closure's 12800 / 200), GREEN with the fix (3200 B / 200 — the
// receiver's residual only), and — after fix #9156 — leak-free.
BEZEL_TEST(indexer_compound_lambda_arg_closure_release_9110,
           imported_compound_assignment_releases_the_index_argument_closure) {
    const std::string cls = "Lib9110C";
    const std::vector<ModuleSource> mods{
        ModuleSource{cls, libModule(cls)},
        ModuleSource{"App9110C", appModule(cls, "fl[lambda() => 7] += 3;")}};
    auto v = runAotModules(mods, "App9110C");
    expectValue(v, 0, "3600\n");
    auto r = runAotModulesChecked(mods, "App9110C");
    expectNoLeak(r);
}

// ── Row 2 (§28.14 / §28.9): the SINGLE-MODULE twin — the compound path's
// inline-setter arm, which leaked identically (the issue's own probe is
// cross-module; this row shows the defect is not an import defect).
//
// RED on the base, GREEN with the fix, leak-free after fix #9156.
BEZEL_TEST(indexer_compound_lambda_arg_closure_release_9110,
           single_module_compound_assignment_releases_the_index_argument_closure) {
    const std::string src =
        singleModule("S9110C", "fl[lambda() => 7] += 3;");
    auto v = runAot(src);
    expectValue(v, 0, "3600\n");
    auto r = runAotChecked(src);
    expectNoLeak(r);
}

// ── Row 3 (GREEN CONTROL — the imported PLAIN setter): #4078's setter twin
// already discharged the index argument's closure producer on the plain
// `fl[lambda() => 7] := 3;` path, so this row is green on the base and green
// with the fix.
//
// It is the DOUBLE-RELEASE guard: a fix that released the producer twice (for
// example one that also released inside the compound's getter half) turns
// this row red with a bad free / use-after-free under ASan.
//
// fix #9156 (§28.2 / §28.3, v0.359): TIGHTENED. This row used to accept the
// receiver's residual as a FLOOR (3200 B / 200 allocations); the residual is
// gone, so the row now asserts NO leak at all. The row NAME is kept — issue
// #9156 and its lane brief refer to it by name.
BEZEL_TEST(indexer_compound_lambda_arg_closure_release_9110,
           imported_plain_setter_stays_at_the_receiver_floor) {
    const std::string cls = "Lib9110S";
    const std::vector<ModuleSource> mods{
        ModuleSource{cls, libModule(cls)},
        ModuleSource{"App9110S", appModule(cls, "fl[lambda() => 7] := 3;")}};
    auto v = runAotModules(mods, "App9110S");
    expectValue(v, 0, "2200\n");
    auto r = runAotModulesChecked(mods, "App9110S");
    expectNoLeak(r);
}

// ── Row 4 (GREEN CONTROL — the imported PLAIN getter): the path #9083 fixed
// (`releaseIndexArgClosureProducers` in `lowerIndexerGet`'s imported arm).
// CLEAN on both arms — no leak at all, not even the receiver's residual: the
// getter path's receiver is a BORROW position, so it was never quarantined
// and its block is reclaimed at the scope end (fix #9156 makes the setter
// path behave the same way).
//
// It is the guard that the compound fix does not release the getter's
// producer a second time: a double release would free the closure while the
// caller still holds it.
BEZEL_TEST(indexer_compound_lambda_arg_closure_release_9110,
           imported_plain_getter_stays_clean) {
    const std::string cls = "Lib9110G";
    const std::vector<ModuleSource> mods{
        ModuleSource{cls, libModule(cls)},
        ModuleSource{"App9110G",
                     "module App9110G; import Lib9110G; "
                     "public class Program9110G "
                     "  public static method Main(): int "
                     "    var t := 0; "
                     "    for var j in 0..<200 do "
                     "      using var fl := new Lib9110G.FlSLib9110G(); "
                     "      t := t + fl[lambda() => 1]; "
                     "    end; "
                     "    Console.WriteLine(t); "
                     "    return 0; "
                     "  end Main; "
                     "end Program9110G;"}};
    auto v = runAotModules(mods, "App9110G");
    expectValue(v, 0, "200\n");
    auto r = runAotModulesChecked(mods, "App9110G");
    expectNoLeak(r);
}

}  // namespace
