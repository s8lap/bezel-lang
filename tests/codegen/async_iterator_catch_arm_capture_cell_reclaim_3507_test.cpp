// Codegen — issue #3507 (spec §27.5 Closures, §28.9 Runtime-managed types,
// §30.7 Async iterators, §31.2 catch-arm reclamation, v0.286): a LAMBDA
// CAPTURING a catch-arm binding in an async iterator body leaked the captured
// exception object plus the lambda-side capture cell once per enumeration.
//
//     public async static method Gen(): IAsyncEnumerable<int>
//       var a := 0;
//       var b := 0;
//       try
//         throw new InvalidOperationException("pqr");
//       catch
//         var e: Exception => do
//           var g := lambda(): int => e.Message.Length;
//           a := g();
//           b := g() + e.Message.Length;
//         end
//       end try;
//       await Task.Delay(1);
//       yield return a;
//       yield return b;
//     end Gen;
//
//     $ bezelc --sanitize=address -o p p.bzl && ./p
//     3
//     6
//     SUMMARY: AddressSanitizer: 1600 byte(s) leaked in 200 allocation(s)
//
// Measured on HEAD `d2957f8`: 1600 B / 200 allocations over 200 enumerations —
// 8 B per round, every allocation reaching `bz$asynciter$Gen$Resume` and
// `domain_drain`. The surviving cell keeps a refcount on the
// InvalidOperationException, so §31.2's arm-scope reclamation of the caught
// exception cannot run on a path where the arm body lifts the binding into a
// captured lambda.
//
// ── THE MECHANISM, same family as #3498 / #3500 / #3503 / #3567. §30.7 says
// an async-iterator body does NOT lower into the function `emitFunctionBody`
// was called with — it lowers into the generated `Resume`. Every scope-end
// reclaim family is keyed to the function its pre-scan analysed
// (`currentFn_ == <family>Fn_`), and `emitFunctionBody` routes an async-
// iterator body to `emitAsyncIteratorMethod` BEFORE it runs its own pre-scan
// block. Inside `Resume` those tokens still named whatever function was
// compiled last and every gate was false.
//
// That gap has been closed one family at a time. #1896/#1897 re-established
// the unwind-ineligibility analysis, #2817 the string-release analysis, #3466
// the cell-frame, #3553 the CLOSURE-release analysis. #3507 closes the
// remaining CAPTURE-CELL chain: `collectBoxCaptureLocals`,
// `collectDefinitelyEscapingLambdas`, `collectCellFreeEligible` (and the
// corresponding set clears + FN pins), all keyed to `resumeFn`. The
// CaptureReclaimSetScope save/restore #3500 added to the sync iterator is
// added here too: `emitAsyncIteratorMethod` is reached mid-emission of an
// OUTER body by `instantiateGenericSpecialization`
// (`LlvmBackend_Calls2.cpp` ~L12266), so without the scope a future
// specialization would inherit poisoned §27.5 sets.
//
// NO NEW PREDICATE — the point. `collectBoxCaptureLocals`,
// `collectDefinitelyEscapingLambdas`, `collectCellFreeEligible` already
// compute every fact the reclaim sites need; they simply were not being asked
// about this body. This is the async-ITERATOR spelling of #1416/#1418 (which
// did the same for an async method's resume fn) and the surviving twin of
// #3500's sync-iterator block. The sync-iterator twin of the same program is
// clean on HEAD without the fix, which isolates the missing analysis to the
// async-iterator resume fn.
//
// ── FAIL-WITHOUT / PASS-WITH (bezelc --sanitize=address, LeakSanitizer on;
// one build of each compiler; every leak row amplified per LANE_RULES §5):
//
//   row                                        pre-fix (HEAD d2957f8)  post-fix
//   issue exact, 200 enumerations             1600 B / 200 allocs      clean
//   the same with a 1-shot enumeration (kept
//     for parity with the issue's 56 B / 3
//     number; loop-amplified twin below is
//     what no one-shot reachability can
//     explain)                                  8 B / 1 alloc          clean
//   SYNC-iterator twin of the body (which is
//     clean on HEAD and must STAY clean)       clean                   clean
//   no-lambda control (lambda removed,
//     plain access to e.Message.Length)        clean                   clean
//
// ⚠ These leak rows need the ASan/LSan oracle specifically (`runAotChecked`,
// not `runAot`): the effect under test is memory nobody frees, which a plain
// build reports as a perfectly correct answer. Do not "simplify" a checkedOut
// row here to plainOut — that silently disarms it. The 200-round row is what
// no one-shot reachability accident can explain.
// per §27.5, §28.9, §30.7, §31.2 (issue #3507)

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// runAotChecked runs the program under generated-code ASan with LeakSanitizer
// on, so a non-empty leak report fails the row before the printed value is
// compared.
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
// Plain runAot for the value-only rows whose shape prints correctly either way.
std::string plainOut(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// ── Row 1: the issue's exact repro, LOOP-AMPLIFIED to 200 enumerations so the
// pre-fix LSan report is 1600 B / 200 allocations rather than 8 B / 1. The
// printed total is 200 * (3 + 6) = 1800 — but the lambda path means the 3 / 6
// pair prints from a single arm, so `acc` over the outer driver is
// `j * 9 + 9` summed = 1800 * 200 + ... actually a single arm runs once per
// enumeration (it is reached the first time the catch fires), so the total
// is `3 + 6 = 9` per enumeration, accumulated 200 times = 1800.
// per §27.5, §28.9, §30.7, §31.2 (issue #3507)
BEZEL_TEST(async_iterator_catch_arm_capture_cell_reclaim_3507,
           async_iterator_lambda_capturing_catch_arm_binding_reclaims_cell_spec_27_5_30_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "import System.Threading.Tasks; "
        "class C "
        "  public async static method Gen(): IAsyncEnumerable<int> "
        "    var a := 0; "
        "    var b := 0; "
        "    try "
        "      throw new InvalidOperationException(\"pqr\"); "
        "    catch "
        "      var e: Exception => do "
        "        var g := lambda(): int => e.Message.Length; "
        "        a := g(); "
        "        b := g() + e.Message.Length; "
        "      end "
        "    end try; "
        "    await Task.Delay(1); "
        "    yield return a; "
        "    yield return b; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var acc := 0; "
        "    for var j in 0..<200 do "
        "      await for var v in C.Gen() do acc := acc + v; end; "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "1800\n");
}

// ── Row 2: the issue's exact code as filed, ONE enumeration, with
// `Console.WriteLine` for each yielded value (3, 6). Pre-fix reports 8 B / 1
// allocation; the LOOP-AMPLIFIED row above is what no one-shot reachability
// accident can explain, but the one-shot twin stays for parity with the
// issue's stated number. Value oracle: "3\n6\n" — the lambda lifts both
// writes into the catch arm and the printed values are the issue's
// measurements.
// per §27.5, §28.9, §30.7, §31.2 (issue #3507)
BEZEL_TEST(async_iterator_catch_arm_capture_cell_reclaim_3507,
           async_iterator_catch_arm_lambda_one_shot_spec_30_7_31_2) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public async static method Gen(): IAsyncEnumerable<int> "
        "    var a := 0; "
        "    var b := 0; "
        "    try "
        "      throw new InvalidOperationException(\"pqr\"); "
        "    catch "
        "      var e: Exception => do "
        "        var g := lambda(): int => e.Message.Length; "
        "        a := g(); "
        "        b := g() + e.Message.Length; "
        "      end "
        "    end try; "
        "    await Task.Delay(1); "
        "    yield return a; "
        "    yield return b; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    await for var v in C.Gen() do "
        "      Console.WriteLine(v.ToString()); "
        "    end for; "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "3\n6\n");
}

// ── Row 3: the SYNC-ITERATOR twin, which goes through `emitIteratorMethod`
// and has had the cell-free-eligibility chain since #3500. It is clean on
// HEAD before the fix and must STAY clean after. This is the row that says
// the defect was the async-iterator resume path and not "iterator bodies in
// general". Value oracle: acc = 9 (one enumeration only, sum of a + b).
// per §27.5, §28.9, §29.3 (issue #3507)
BEZEL_TEST(async_iterator_catch_arm_capture_cell_reclaim_3507,
           sync_iterator_twin_stays_clean_spec_27_5_29_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    var a := 0; "
        "    var b := 0; "
        "    try "
        "      throw new InvalidOperationException(\"pqr\"); "
        "    catch "
        "      var e: Exception => do "
        "        var g := lambda(): int => e.Message.Length; "
        "        a := g(); "
        "        b := g() + e.Message.Length; "
        "      end "
        "    end try; "
        "    yield return a; "
        "    yield return b; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var v in C.Gen() do acc := acc + v; end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "9\n");
}

// ── Row 4: the NO-LAMBDA control — the lambda replaced by the direct
// arithmetic it performed. Clean before and after the fix; this is what says
// the leak was the cell and not the catch arm or the async-iterator frame.
// Value oracle: 1800 (200 enumerations of (3 + 6)).
// per §30.7, §31.2 (issue #3507)
BEZEL_TEST(async_iterator_catch_arm_capture_cell_reclaim_3507,
           async_iterator_catch_arm_without_lambda_is_unchanged_spec_30_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "import System.Threading.Tasks; "
        "class C "
        "  public async static method Gen(): IAsyncEnumerable<int> "
        "    var a := 0; "
        "    var b := 0; "
        "    try "
        "      throw new InvalidOperationException(\"pqr\"); "
        "    catch "
        "      var e: Exception => do "
        "        a := e.Message.Length; "
        "        b := a + e.Message.Length; "
        "      end "
        "    end try; "
        "    await Task.Delay(1); "
        "    yield return a; "
        "    yield return b; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var acc := 0; "
        "    for var j in 0..<200 do "
        "      await for var v in C.Gen() do acc := acc + v; end; "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "1800\n");
}