// Codegen — a NESTED lambda's own re-scan must not elect a #3426 shared-cell
// envdtor owner for a cell an ANCESTOR frame allocates (§27.5 / §28.9, #4524).
//
// Spec §27.5: "A lambda may reference variables from its enclosing scope.
// Captured variables are heap-allocated in a closure object." Spec §28.9:
// function-typed values' "lifecycle ... is not part of this specification" —
// so what is pinned here is not a spec-stated reclaim schedule but the one
// property every reclaim schedule must have: the cell is freed AT MOST ONCE,
// and never while a live closure can still read through it.
//
// THE DEFECT. `collectCellFreeEligible`'s #3426 election names the ONE
// escaping lambda whose envdtor is the cell's last reader. Its soundness
// argument is an argument about ONE frame: every non-escaping co-capturer's
// env is released at a scope end INSIDE this frame, the escaping holder's env
// dies strictly after this frame is gone, therefore that envdtor is the last
// reader. That reasoning describes the CELL's lifetime only while the walked
// body is the frame that ALLOCATES the cell.
//
// `lowerLambda` re-runs the whole pre-scan for every nested lambda's own body
// (fix #3757's re-pin). A nested body cannot see its own call sites, so it
// elects happily for an ancestor's local:
//
//     var x := 11;                      (* Run allocates x's cell *)
//     var l1 := lambda() => do
//       var l2 := lambda() => do
//         var l3 := lambda() => x;      (* l1 elects l3 as sole owner *)
//         return l3;
//       end;
//       return l2();
//     end;
//     var a  := l1();                   (* first l3 closure  *)
//     var b  := a();
//     return l1();                      (* second l3 closure, same cell *)
//
// `Run`'s own walk DECLINES correctly — #3804's `calledAtMostOnce` reads
// false for `l1`, which has two call sites. But `l1`'s re-scan then
// RE-ELECTS, because inside `l1`'s body `l2` really is called exactly once.
// Two `l3` envs over ONE cell, each envdtor freeing it. Measured on
// origin/main (6c05b4cf), `bezelc --sanitize=address`:
//
//     ERROR: AddressSanitizer: heap-use-after-free
//     READ of size 4 ... in lam$2
//     freed by thread T0 here: ... bz$lam$2$envdtor
//     previously allocated by thread T0 here: ... bz$M.C$Run
//
// and, with the intervening read removed (row 2), `attempting double-free`.
//
// WHY THE BOUNDARY SITS BETWEEN TWO AND THREE LEVELS. The 2-level form of the
// very same program is `codegen_deeply_nested_escaping_lambda_3804`'s own
// soundness row 3 (`outer_called_twice_one_escapes_no_uaf`) and is CLEAN:
// there the declaring frame IS the only frame that walks, so its refusal
// stands. Add one nesting level and the refusal is overruled by a body that
// does not own the cell. Nothing pinned that boundary before this file.
//
// THE FIX is conjunct (f) of the election: only the ALLOCATING frame may
// elect. A key the walked body does not itself bind gets no election in a
// nested re-scan; the declaring frame's own verdict still reaches that body
// through #3804's `ancestorSharedCellOwner3804` merge, which is exactly what
// the merge exists for — so the wrong answer is removed without removing the
// right one. Row 5 is the guard on that claim.
//
// ORACLE. `runAotCheckedLenient` (generated-code ASan live, LSan off) is the
// right oracle and not a weakened one: the fix converts an over-free into the
// pre-existing conservative leak, which is the ACCEPTED trade-off #3804 row 3
// already pins for the 2-level twin ("the pre-existing conservative leak ...
// is the ACCEPTED trade-off this row is pinning, not a defect"). Post-fix the
// leak is bounded at one 4-byte cell per `Run` call — measured 8 B / 2
// allocations for row 1's two calls, 2000 B / 500 allocations amplified x500.
// Asserting leak-freedom here would assert something this fix does not claim
// and #4509's follow-up owns. Row 5 keeps a STRICT leak oracle exactly where
// this fix must not have cost anything.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// The three-level chain of #4524's own repro, parameterised on what the
// chain captures and on what `Main` does with the escaped closure.
std::string threeLevelProgram(const std::string& runSignature,
                              const std::string& declareX,
                              const std::string& mainBody) {
    return
        "module M; "
        "class C "
        "  public static method Run(" + runSignature + "): () -> int " +
        declareX +
        "    var l1 := lambda() => do "
        "      var l2 := lambda() => do "
        "        var l3 := lambda() => x; "
        "        return l3; "
        "      end; "
        "      return l2(); "
        "    end; "
        "    var a := l1(); "
        "    var b: int := a(); "
        "    return l1(); "
        "  end Run; "
        "  public static method Main(): int " + mainBody +
        "  end Main; "
        "end C;";
}

}  // namespace

// ============================================================================
// Row 1 (THE UAF): #4524's repro verbatim. `Run` declares `x`, three nested
// lambdas capture it, `l1()` is called twice and the second result escapes to
// `Main`, which reads `x` through it STRICTLY AFTER `Run` has returned — and
// after `a`'s envdtor has already freed the cell.
//
// Pre-fix (origin/main 6c05b4cf): `heap-use-after-free ... READ of size 4 in
// lam$2 / freed by bz$lam$2$envdtor` — the harness reports ran=false.
// Post-fix: runs, and both closures still read x == 11, so 11 + 11 == 22.
// ============================================================================
BEZEL_TEST(codegen_capture_cell_nested_rescan_election_4524,
           three_level_chain_called_twice_no_uaf) {
    std::string src = threeLevelProgram(
        /*runSignature=*/"",
        /*declareX=*/"    var x := 11; ",
        /*mainBody=*/
        "    var e1 := C.Run(); "
        "    var e2 := C.Run(); "
        "    var n: int := e1() + e2(); "
        "    return n; ");
    auto r = runAotCheckedLenient(src);
    BEZEL_EXPECT_TRUE(r.ran);       // ASan aborts on the pre-fix UAF
    BEZEL_EXPECT_EQ(r.exitCode, 22);
}

// ============================================================================
// Row 2 (THE OVER-RELEASE, the worse half): the same program with the read
// through the escaped closure REMOVED, so the second envdtor runs over an
// already-freed cell with no intervening load. Row 1 catches the UAF because
// the read happens first; this row catches the free itself.
//
// Pre-fix: `AddressSanitizer: attempting double-free` (measured on
// origin/main 6c05b4cf). Post-fix: runs to completion, exit 0.
//
// This row exists because "a freed-cell-still-read is worse than the leak"
// cuts both ways: a fix that merely reordered the read would leave the
// double-free standing, and row 1 alone could not tell the difference.
// ============================================================================
BEZEL_TEST(codegen_capture_cell_nested_rescan_election_4524,
           three_level_chain_called_twice_no_double_free) {
    std::string src = threeLevelProgram(
        /*runSignature=*/"",
        /*declareX=*/"    var x := 11; ",
        /*mainBody=*/
        "    var e1 := C.Run(); "
        "    return 0; ");
    auto r = runAotCheckedLenient(src);
    BEZEL_EXPECT_TRUE(r.ran);       // ASan aborts on the pre-fix double-free
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ============================================================================
// Row 3 (THE PARAMETER KEY SPACE): the identical chain over a captured
// PARAMETER instead of a local. `sharedCellEnvdtorFreeOwner_` keys parameters
// by `sema::ParameterSymbol*` and locals by their decl node, and the election
// treats the two key spaces alike — so the same nested re-scan elects the same
// wrong owner. Measured pre-fix: the same `heap-use-after-free ... in lam$2`.
//
// Kept as its own row because the fix's own-frame set is built from two
// different sources for the two key spaces (the walker's LocalDecl arms for
// locals, `ctx_.lambdaParams` for a nested lambda's formals); a fix that
// covered only one would pass row 1 and fail here.
// ============================================================================
BEZEL_TEST(codegen_capture_cell_nested_rescan_election_4524,
           three_level_chain_over_parameter_no_uaf) {
    std::string src = threeLevelProgram(
        /*runSignature=*/"x: int",
        /*declareX=*/"",
        /*mainBody=*/
        "    var e1 := C.Run(11); "
        "    var e2 := C.Run(11); "
        "    var n: int := e1() + e2(); "
        "    return n; ");
    auto r = runAotCheckedLenient(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 22);
}

// ============================================================================
// Row 4 (DEPTH IS NOT THE PREDICATE): four levels. The defect is not "three
// levels are special" — it is "a body that does not allocate the cell got to
// decide". Any depth at which a non-allocating body's walk can see a
// single-call chain reproduces it, and a fix keyed on a depth count rather
// than on frame ownership would pass rows 1-3 and fail here.
//
// Measured pre-fix: `heap-use-after-free` (same shape). Post-fix: 22.
// ============================================================================
BEZEL_TEST(codegen_capture_cell_nested_rescan_election_4524,
           four_level_chain_called_twice_no_uaf) {
    std::string src =
        "module M; "
        "class C "
        "  public static method Run(): () -> int "
        "    var x := 11; "
        "    var l1 := lambda() => do "
        "      var l2 := lambda() => do "
        "        var l3 := lambda() => do "
        "          var l4 := lambda() => x; "
        "          return l4; "
        "        end; "
        "        return l3(); "
        "      end; "
        "      return l2(); "
        "    end; "
        "    var a := l1(); "
        "    var b: int := a(); "
        "    return l1(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var e1 := C.Run(); "
        "    var e2 := C.Run(); "
        "    var n: int := e1() + e2(); "
        "    return n; "
        "  end Main; "
        "end C;";
    auto r = runAotCheckedLenient(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 22);
}

// ============================================================================
// Row 5 (THE NARROWING DID NOT COST ANYTHING — STRICT LEAK ORACLE,
// AMPLIFIED x500): #3804's own block-bodied probe, where the election is made
// by `Make` — the frame that DECLARES `x` — and must survive untouched. This
// is the boundary the fix could plausibly have overshot: declining the
// election everywhere, rather than only in a body that does not own the cell,
// would restore #3804's leak (26000 B / 1500 allocations at x500, pre-#3804).
//
// The oracle here is deliberately the STRICT one (`runAotChecked`, LSan on)
// and the loop is 500 iterations, because a one-shot LSan row over a 4-byte
// cell passes vacuously on both sides — the allocation stays reachable from a
// live stack slot. Post-fix: clean (measured 0 leaks at x500).
// ============================================================================
BEZEL_TEST(codegen_capture_cell_nested_rescan_election_4524,
           declaring_frame_election_still_reclaims_leak_amplified_500x) {
    std::string src =
        "module M; import System; "
        "class C "
        "  public static method Make(): () -> int "
        "    var x := 3; "
        "    var outer := lambda() => do "
        "      var inner := lambda() => x; "
        "      return inner; "
        "    end; "
        "    return outer(); "
        "  end Make; "
        "  public static method Run(): int "
        "    var f := C.Make(); "
        "    return f(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..500 do "
        "      total := total + C.Run(); "
        "    end; "
        "    Console.WriteLine(total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
