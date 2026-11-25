// Regression test for issue #9210 — a CONDITIONAL whose arms are LAMBDAS,
// handed to a call, WILD-FREED the capture cell of the variable only ONE arm
// captures.
//
// THE DEFECT. §27.5 (v0.359) gives a captured variable ONE cell per binding:
// the closure shares the variable's storage through a heap cell, and the
// DECLARING FRAME owns a reference to that cell, which it drops at the
// declaring scope's end (§28.9: "Runtime management governs the *function
// value itself* — the heap-allocated closure object that backs it"; the cell
// is the function value's captured storage, §27.5). The cell's pointer lives
// in a HOLDER alloca, and the holder is written ONLY at the capture site —
// the point where the cell is allocated. Every scope-end reclaim NULLS the
// holder afterwards, deliberately, "leaving `boxLocalSlot`'s guard to allocate
// a fresh cell on the next capture".
//
// A scope whose capture site is CONDITIONAL therefore reaches its reclaim on a
// path where NO cell was ever formed: there the holder holds no cell at all.
// `boxLocalSlot` (src/codegen/LlvmBackend_Layout.cpp) has two strategies. The
// GUARDED one null-initialises its holder in the entry block, so "no cell"
// reads as NULL and the reclaim is a no-op (`free(null)` /
// `bz_cell_release(null)`). The FRESH one — a local declared INSIDE a loop
// body, or one re-initialised on every pass — leaves a BARE `entryAlloca`, so
// on a path that never reached the capture site the holder holds UNDEF.
//
// fix #3484's repair (extended by fix #4612) gives every EXPOSED holder read a
// null fall-back AND null-initialises the holder it guards — but only for the
// reads `boxedStorage` ENROLLS, and a scope-end RECLAIM is not one of them:
// it read the holder RAW. So on the first pass that skipped the capture site
// the reclaim released an uninitialised pointer.
//
// MEASURED on the base (r30-c1 @ 702fb3d9c), the issue's own repro over 200
// iterations under `bezelc --sanitize=address`:
//
//   ==905397==ERROR: AddressSanitizer: SEGV on unknown address
//       (pc 0x000000282db7 ...)
//     #0 __asan::Allocator::Deallocate(void*, unsigned long, unsigned long, ...)
//     #1 free
//     #2 main
//   SUMMARY: AddressSanitizer: SEGV in __asan::Allocator::Deallocate
//
// gdb on that binary pins the instruction (the reclaim's own load of the
// holder, feeding `bz_cell_release`, which tail-calls `free`):
//
//   3740c5:  mov 0x40(%rbx),%rdi
//   3740c9:  call 3a3950 <bz_cell_release>
//   3740ce:  movq $0x0,0x40(%rbx)
//
// with `rdi` = 0x5756555453525150 — ASCII "PQSRSTUV", i.e. STACK RESIDUE, not
// a heap allocation. A wild free of a pointer that was never a heap object.
//
// The issue's own hypothesis ("a refcount / ownership-shape mismatch between
// the two arms' closure cells") is MEASURED to be a different thing: the
// closure cells themselves are fine (the program's output is correct on every
// shape that does not crash). The mismatch is between the two arms in the
// CAPTURE shape — one arm captures a variable and forms a cell, the other
// does not — and the scope-end release of the capture cell fires whether or
// not the capturing arm ran.
//
// THE FIX. The scope-end reclaim of a capture cell now reads the holder
// through the SAME §27.5 funnel the value reads use, with a NULL fall-back:
// "the cell, or NOTHING" is what a reclaim wants when the capture site never
// ran, and the pre-boxing storage — the fall-back the value reads use — is
// not a cell at all and must never be freed. The fall-back being a CONSTANT
// also means the read needs no state-machine round-trip.
//
// The fix is a NO-OP for a capture site that always runs: the repair leaves a
// read whose holder-completion block DOMINATES it untouched, so every
// unconditional-capture program keeps byte-identical IR (the same discipline
// fix #3484 established for the value reads). The control rows at the end of
// this file pin that.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. Every row asserts
// the program's own output AND that LeakSanitizer counted nothing, so a
// silent zero-fill cannot pass and no double free can hide. The rows LOOP (200
// iterations inside the program), so nothing can hide under the sanitizer's
// noise. Lane-contract §5 requires the checked oracle for disposal /
// ownership work, and the neighbouring #9170 rows use it for the same reason.
//
// `runAot` CANNOT be the oracle here: it does not run the AddressSanitizer
// pass over the generated module (only `runAotChecked` sets
// `opts.sanitizeAddress`), so a wild free would be reported as a crash
// without the leak/double-free leg — the checked oracle verifies BOTH that the
// defect is gone AND that nothing now double-frees.
//
// per §27.5 / §28.3 / §28.9 / §28.14 (issue #9210; siblings #3484, #4612,
// #4509, #1017, #823)

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
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

// The program ran to completion: it compiled, the sanitizer leg really ran (a
// toolchain SKIP fails the row instead of passing it silently), it exited
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

}  // namespace

// ROW 1 — THE ISSUE'S LITERAL REPRO. A CONDITIONAL of two lambdas, handed
// DIRECTLY as a call argument to a function-typed formal
// (`Get(q: () -> int)`). One arm captures the loop-body local `x`, the other
// does not. The callee BORROWS the closure (§28.14: a function-typed
// parameter without `scoped` is call-bounded), so the call site owns the
// value; §27.5's cell is the frame's own reference and is dropped at the
// declaring scope's end.
//
// The total is 101 x 8 + 99 x 5 = 1303: the `else` arm (8) is taken for
// i = 0..100 and the `then` arm (x = 5) for i = 101..199.
//
// RED on the base: SEGV in __asan::Allocator::Deallocate — a wild free of the
// capture cell's holder, read as stack residue. GREEN with the fix.
BEZEL_TEST(conditional_lambda_arm_capture_cell_release_9210,
           conditional_lambda_arms_passed_as_call_argument_do_not_wild_free) {
    const std::string src =
        "module M; import System; "
        "public class Fl "
        "  public constructor() end constructor; "
        "  public method Get(q: () -> int): int "
        "    var v := q(); "
        "    return v; "
        "  end Get; "
        "end Fl; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var fl := new Fl(); "
        "      var x := 5; "
        "      var f := fl.Get((if i > 100 then lambda() => x else lambda() => 8)); "
        "      t := t + f; "
        "    end for; "
        "    Console.WriteLine($\"{t}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1303\n");
}

// ROW 2 — THE SAME CONDITIONAL, BOUND TO A LOCAL FIRST. The issue names this
// shape as the discriminator to check, and it is MEASURED to crash on the base
// exactly like ROW 1: the crash is in the CAPTURE CELL's scope-end reclaim,
// not in anything about the call's argument position. So the argument
// position is NOT what makes the defect — the conditional's two arms having
// DIFFERENT CAPTURE SHAPES is.
//
// The total is the same 101 x 8 + 99 x 5 = 1303.
//
// RED on the base: SEGV in __asan::Allocator::Deallocate. GREEN with the fix.
BEZEL_TEST(conditional_lambda_arm_capture_cell_release_9210,
           conditional_lambda_arms_bound_to_local_first_do_not_wild_free) {
    const std::string src =
        "module M; import System; "
        "public class Fl "
        "  public constructor() end constructor; "
        "  public method Get(q: () -> int): int "
        "    var v := q(); "
        "    return v; "
        "  end Get; "
        "end Fl; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var x := 5; "
        "      var g := (if i > 100 then lambda() => x else lambda() => 8); "
        "      var fl := new Fl(); "
        "      var f := fl.Get(g); "
        "      t := t + f; "
        "    end for; "
        "    Console.WriteLine($\"{t}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1303\n");
}

// ROW 3 — ONE LAMBDA ARM + ONE METHOD-GROUP ARM. The second shape the #9170
// review (r43-rev-m95-9170) reported on this issue: "a conditional whose arms
// are a method group and a capturing lambda ... crashes with the same wild
// free in `__asan::Allocator::Deallocate`". §27.6 forms a method reference as
// a FUNCTION VALUE too, so the arm that names it captures NOTHING and the arm
// that is a lambda captures `x` — the same two capture shapes.
//
// The total is 101 x 4 + 99 x 5 = 899: the `else` arm (the method group
// `q.Val`, 4) is taken for i = 0..100 and the `then` arm (x = 5) for
// i = 101..199.
//
// RED on the base: SEGV in __asan::Allocator::Deallocate. GREEN with the fix.
BEZEL_TEST(conditional_lambda_arm_capture_cell_release_9210,
           conditional_lambda_arm_and_method_group_arm_do_not_wild_free) {
    const std::string src =
        "module M; import System; "
        "public class Q "
        "  public n: int; "
        "  public constructor(n: int) this.n := n; end constructor; "
        "  public method Val(): int return this.n; end Val; "
        "end Q; "
        "public class Fl "
        "  public constructor() end constructor; "
        "  public method Get(q: () -> int): int "
        "    var v := q(); "
        "    return v; "
        "  end Get; "
        "end Fl; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var x := 5; "
        "      var q := new Q(4); "
        "      var fl := new Fl(); "
        "      var f := fl.Get((if i > 100 then lambda() => x else q.Val)); "
        "      t := t + f; "
        "    end for; "
        "    Console.WriteLine($\"{t}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "899\n");
}

// ROW 4 — BOTH ARMS CAPTURE, each a DIFFERENT loop-body local. Neither arm is
// capture-free, so this row is NOT the "capturing + non-capturing" shape: it
// shows that the defect is about the arms having DIFFERENT capture shapes, not
// about one of them being capture-free. The two locals have two holders, and
// only the holder of the arm that ran was ever written — so the OTHER holder
// is the one the reclaim reads as UNDEF.
//
// The total is 101 x 7 + 99 x 5 = 1202: the `else` arm (y = 7) is taken for
// i = 0..100 and the `then` arm (x = 5) for i = 101..199.
//
// RED on the base: SEGV in __asan::Allocator::Deallocate. GREEN with the fix.
BEZEL_TEST(conditional_lambda_arm_capture_cell_release_9210,
           conditional_with_two_capturing_arms_do_not_wild_free) {
    const std::string src =
        "module M; import System; "
        "public class Fl "
        "  public constructor() end constructor; "
        "  public method Get(q: () -> int): int "
        "    var v := q(); "
        "    return v; "
        "  end Get; "
        "end Fl; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var x := 5; "
        "      var y := 7; "
        "      var fl := new Fl(); "
        "      var f := fl.Get((if i > 100 then lambda() => x else lambda() => y)); "
        "      t := t + f; "
        "    end for; "
        "    Console.WriteLine($\"{t}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1202\n");
}

// ROW 5 — THE SAME DEFECT, THE OTHER RECLAIM ARM. A lambda that captures a
// loop-body local behind a plain `if` BODY (not an if-EXPRESSION arm). The
// scope-end reclaim that fires here is #1017's DEFERRED free (a direct
// `free` of the cell), not #4509's `cellRelease` — so this row pins the
// SECOND of the three reclaim sites that read the holder, and keeps the fix
// honest for both.
//
// The total is 99 x 5 = 495: the `if` body runs for i = 101..199 only.
//
// RED on the base: SEGV in __asan::Allocator::Deallocate. GREEN with the fix.
BEZEL_TEST(conditional_lambda_arm_capture_cell_release_9210,
           loop_body_local_captured_behind_if_does_not_wild_free) {
    const std::string src =
        "module M; import System; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var x := 5; "
        "      if i > 100 then "
        "        var g := lambda() => x; "
        "        t := t + g(); "
        "      end if; "
        "    end for; "
        "    Console.WriteLine($\"{t}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "495\n");
}

// CONTROL 1 — THE UNCONDITIONAL CAPTURE. The same program with the
// conditional REMOVED, so the capture site runs on every pass and the holder
// is written before every reclaim. This is the shape the fix must leave
// byte-identical (the repair's dominance test leaves a dominated read
// untouched), so it pins that the fix does not add a guard — and a null
// store — to the unconditional family.
//
// GREEN on the base and with the fix. RED under a plausible wrong fix — one
// that discharges the cell a SECOND time (an unconditional release beside the
// guarded one, or a release per ARM): the cell would be freed twice and
// AddressSanitizer would report a double free.
BEZEL_TEST(conditional_lambda_arm_capture_cell_release_9210,
           unconditional_capture_control_stays_clean) {
    const std::string src =
        "module M; import System; "
        "public class Fl "
        "  public constructor() end constructor; "
        "  public method Get(q: () -> int): int "
        "    var v := q(); "
        "    return v; "
        "  end Get; "
        "end Fl; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var x := 5; "
        "      var fl := new Fl(); "
        "      var f := fl.Get(lambda() => x); "
        "      t := t + f; "
        "    end for; "
        "    Console.WriteLine($\"{t}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1000\n");
}

// CONTROL 2 — THE CAPTURED LOCAL DECLARED OUTSIDE THE LOOP. `boxLocalSlot`'s
// FRESH strategy has a second arm for a local declared outside the loop: it
// allocates the holder with an entry-block null store (`prevHolder`), so "no
// cell" already reads as NULL there and the reclaim was never reached by the
// defect. This row pins that the fix did not change that arm's behaviour.
//
// GREEN on the base and with the fix. RED under a plausible wrong fix — one
// that releases the holder's previous value at the capture site without
// checking it (fix #8949's `free(old)` discipline) or that nulls the holder
// before the reclaim: the cell would be freed twice.
BEZEL_TEST(conditional_lambda_arm_capture_cell_release_9210,
           capture_declared_outside_the_loop_control_stays_clean) {
    const std::string src =
        "module M; import System; "
        "public class Fl "
        "  public constructor() end constructor; "
        "  public method Get(q: () -> int): int "
        "    var v := q(); "
        "    return v; "
        "  end Get; "
        "end Fl; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    var x := 5; "
        "    for var i in 0 ..< 200 do "
        "      var fl := new Fl(); "
        "      var f := fl.Get((if i > 100 then lambda() => x else lambda() => 8)); "
        "      t := t + f; "
        "    end for; "
        "    Console.WriteLine($\"{t}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1303\n");
}
