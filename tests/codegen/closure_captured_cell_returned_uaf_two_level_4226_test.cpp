// Codegen: a closure-typed local captured/reassigned TWO levels deep (a
// lambda nested inside another lambda), then returned by name from the
// enclosing function, is a heap-use-after-free unaffected by #4198's own fix
// (issue #4226).
//
// WHAT #4198 FIXED, AND WHERE IT STOPPED. #4198 (closure_captured_cell_
// returned_uaf_4198_test.cpp) fixed the ONE-level case: a captured local
// independently RETURNED BY NAME from the enclosing function is a heap-use-
// after-free, because the envdtor's content release and the return's silent
// transfer both claim the SAME single reference (§27.5 / §28.9). Its fix,
// `Emitter::returnedCaptureVars_` (LlvmBackend_impl.hpp) /
// `capturedLocalReturnedByName`, is populated by `collectBoxCaptureLocals`'s
// `scanReturn` walk over the ENCLOSING function's own top-level statements —
// deliberately NOT descending into lambda bodies, since a nested lambda's own
// `return` escapes THAT lambda, not the enclosing function.
//
// THE GAP #4226 FOUND. `lowerLambda` (LlvmBackend_Exprs2.cpp) re-invokes
// `collectBoxCaptureLocals` for EVERY block-bodied lambda's OWN body, to
// discover what ITS nested lambdas need to capture — and that call
// unconditionally `.clear()`s `returnedCaptureVars_` before rescanning. For
//
//     var f: () -> int := lambda() => 0;
//     var outer := lambda() => do
//       var inner := lambda() => do
//         f := lambda() => 42;
//       end;
//       inner();
//     end;
//     outer();
//     return f;                    // <- `f` is returned by MakeClosure, not by `outer`
//
// `MakeClosure`'s own top-level scan correctly puts `f` in
// `returnedCaptureVars_` (so `outer`'s OWN capture-loop entry for `f`, read
// BEFORE `outer`'s body is processed, correctly suppresses `outer`'s envdtor
// release). But lowering `outer`'s OWN body re-invokes
// `collectBoxCaptureLocals(outer.stmts)`, which clears `returnedCaptureVars_`
// and rescans ONLY `outer`'s own top-level statements for `return` — finding
// NONE (outer never itself returns `f`) — so by the time `inner` (nested
// inside `outer`'s body) has ITS OWN capture-loop entry for `f` decided, the
// set is EMPTY, and `inner`'s own envdtor is left unsuppressed: it frees the
// cell's content while `MakeClosure` is still executing, well before
// `return f;` — measured (this run's r18-integration base, and reproduced
// identically on 6f46a73 with #4198's own fix present):
//
//     ==ERROR: AddressSanitizer: heap-use-after-free ... in main
//     freed by thread T0 here:
//         #1 ... in bz$lam$2$envdtor bezel
//     previously allocated by thread T0 here:
//         #1 ... in lam$2 bezel
//
// (`lam$2` = `inner`; matches the issue's own `nm` numbering exactly: `lam$0`
// = f's initializer, `lam$1` = outer, `lam$2` = inner, `lam$3` = the
// reassigned value.)
//
// THE FIX. Snapshot `returnedCaptureVars_` immediately before
// `lowerLambda` clears it for a nested lambda's own rescan, and merge that
// snapshot back in (plain set union — a pure membership fact, so a union is
// always sound) immediately after the rescan, for BOTH the block-bodied and
// expression-bodied branches. This is the EXACT pattern issue #3804 already
// established for `sharedCellEnvdtorFreeOwner_` in this SAME function, for
// the SAME reason: an ancestor scope's already-proven fact about a captured
// free variable is invisible to a fresh walk over just the nested lambda's
// own statements, because the free variable is not itself declared there.
// Because the snapshot/merge pair runs on every `lowerLambda` invocation, the
// fact accumulates correctly through ANY number of nesting levels (see the
// THREE-level row below), not just two.
//
// WHAT WAS EXPECTED BUT NOT FOUND. `capturedLocalReturnedByName`'s trade,
// like `mutatedSharedCaptureCell`'s (#2706), is "suppress the retain AND the
// release together, on a bounded leak instead of an over-release" — so a
// leak from #4227 (a captured cell's displaced value on reassignment is never
// released) was expected on the MUTATED rows below, mirroring #4198's own row
// A. Measured (this fix, `--probe --asan` and this file's own `runAotChecked`
// rows): NO leak on any row here, including the mutated ones — because the
// values assigned to `f` in these programs (`lambda() => 42` / `=> 99`) are
// CAPTURELESS closures, which this codegen represents as a value with a null
// env pointer (no heap allocation at all), so there is nothing for #4227's
// gap to leak in these specific shapes. This does not contradict #4198's own
// row A (which measured a real 40-byte leak on an equivalent single-level,
// same-literal reassignment) — see this file's own row (A) comment for the
// direct, mechanical resolution: 4198's row A uses `runAotCheckedLenient`
// only because the RETURNED value there is later itself disposed via a
// different path (the temporary `g` at end of `Main`), not because the
// reassignment itself leaks. All rows below run fully clean (`runAotChecked`,
// LSan ON) precisely because they were MEASURED clean, not assumed so.
//
// ALSO FOUND, FILED SEPARATELY (both out of #4226's own stated scope):
//   - issue #4239: `return SomeMethodReturningAClosure()();` (calling the
//     RESULT of a closure-returning call immediately, without binding it to
//     a local first) double-frees the temporary — reproduces identically at
//     ONE level of nesting (no lambda-in-lambda at all), so it is unrelated
//     to this fix; every row below binds the returned closure to a `var`
//     first specifically to avoid this orthogonal hazard.
//   - issue #4240: a captured-and-MUTATED (not returned) cell shared across
//     TWO levels of nesting (`outer` transitively passing `f` down to
//     `inner`, which reassigns it) is STILL a heap-use-after-free — a
//     `singleCaptureLocals_`/`mutatedSharedCaptureCell` undercount at 2+
//     levels (the same "outer doesn't count itself as a captor of what it
//     hands down" blind spot, one mechanism over from this fix's own
//     `returnedCaptureVars_`). NOT fixed here: it is a different root cause
//     (mutation-sharing, not return-by-name) and reproduces with no `return`
//     of `f` anywhere in the program, so `capturedLocalReturnedByName` is not
//     even consulted for it. Row (D) below is the READ-ONLY analogue (no
//     mutation), which stays correctly crash-free — the working control this
//     bug's absence needs.
//
// §27.5 ("Closures" — "Modifications to a captured variable inside the
// lambda are visible outside, and vice versa") is the only genuine spec
// citation here — the return-vs-capture refcounting interaction itself has
// no spec authority: §28.9 explicitly disclaims the refcounting
// IMPLEMENTATION ("The implementation of runtime-managed types' lifecycle
// ... is not part of this specification"), so, like #4198's own fix, this is
// an INTERNAL soundness fix, not an implementation of a specific spec rule.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"   // runAot / runAotChecked(Lenient) — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::checkedToolchainSkipped;
using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

}  // namespace

// =====================================================================
// (A) The issue's own repro, bound to a local before calling (avoiding the
// unrelated #4239 double-free on a chained `Foo()()` call): a lambda
// (`inner`) nested inside another lambda (`outer`) reassigns `f`, which is
// then returned by the function that declares both `outer` and `f`. Pre-fix:
// heap-use-after-free, freed by `inner`'s own envdtor, in `main` (matches the
// issue's measured trace exactly). Post-fix: correct value, fully clean —
// see the file header for why no #4227-class leak appears in this shape.
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_returned_uaf_two_level_4226,
           mutated_two_level_nested_capture_returned_no_crash_correct_value_spec_27_5) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method MakeClosure(): () -> int "
        "    var f: () -> int := lambda() => 0; "
        "    var outer := lambda() => do "
        "      var inner := lambda() => do "
        "        f := lambda() => 42; "
        "      end; "
        "      inner(); "
        "    end; "
        "    outer(); "
        "    return f; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var g := C.MakeClosure(); "
        "    return g(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);          // pre-fix: heap-use-after-free in main
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// =====================================================================
// (A2) The same shape, loop-amplified to 500 calls — the crash-freedom and
// memory-cleanliness claim at scale, under the FULL LeakSanitizer oracle.
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_returned_uaf_two_level_4226,
           mutated_two_level_nested_capture_returned_x500_no_crash_spec_27_5) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method MakeClosure(): () -> int "
        "    var f: () -> int := lambda() => 0; "
        "    var outer := lambda() => do "
        "      var inner := lambda() => do "
        "        f := lambda() => 42; "
        "      end; "
        "      inner(); "
        "    end; "
        "    outer(); "
        "    return f; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var acc := 0; "
        "    while i < 500 do "
        "      var g := C.MakeClosure(); "
        "      acc := acc + g(); "
        "      i := i + 1; "
        "    end; "
        "    if acc <> 21000 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// (B) READ-ONLY two-level capture (no mutation anywhere): `inner` merely
// reads `f`, and `f` is returned by the function that declares `outer`.
// Refutes any notion that the two-level hazard needs a WRITE — same
// argument as #4198's own read-only row, one nesting level deeper.
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_returned_uaf_two_level_4226,
           read_only_two_level_nested_capture_returned_fully_clean_spec_27_5) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method MakeClosure(): () -> int "
        "    var f: () -> int := lambda() => 7; "
        "    var outer := lambda() => do "
        "      var inner := lambda() => do "
        "        var x := f(); "
        "      end; "
        "      inner(); "
        "    end; "
        "    outer(); "
        "    return f; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var g := C.MakeClosure(); "
        "    return g(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// =====================================================================
// (B2) Same read-only two-level shape, loop-amplified to 500 calls under the
// full LeakSanitizer oracle.
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_returned_uaf_two_level_4226,
           read_only_two_level_nested_capture_returned_x500_is_memory_clean_spec_27_5) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method MakeClosure(): () -> int "
        "    var f: () -> int := lambda() => 7; "
        "    var outer := lambda() => do "
        "      var inner := lambda() => do "
        "        var x := f(); "
        "      end; "
        "      inner(); "
        "    end; "
        "    outer(); "
        "    return f; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var acc := 0; "
        "    while i < 500 do "
        "      var g := C.MakeClosure(); "
        "      acc := acc + g(); "
        "      i := i + 1; "
        "    end; "
        "    if acc <> 3500 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// (C) THREE levels of nesting (`l1` -> `l2` -> `l3`, `l3` reassigns `f`) —
// proves the ancestor snapshot/merge accumulates correctly through more than
// one intermediate scope, not just the two-level shape the issue reported.
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_returned_uaf_two_level_4226,
           mutated_three_level_nested_capture_returned_no_crash_spec_27_5) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method MakeClosure(): () -> int "
        "    var f: () -> int := lambda() => 0; "
        "    var l1 := lambda() => do "
        "      var l2 := lambda() => do "
        "        var l3 := lambda() => do "
        "          f := lambda() => 99; "
        "        end; "
        "        l3(); "
        "      end; "
        "      l2(); "
        "    end; "
        "    l1(); "
        "    return f; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var g := C.MakeClosure(); "
        "    return g(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 99);
}

// =====================================================================
// (D) OVER-CORRECTION GUARD — read-only two-level capture, `f` is NEVER
// returned by name (only invoked, 9 times). `capturedLocalReturnedByName`
// must NOT fire (returnedCaptureVars_ never contains `f`'s decl for this
// program at any nesting level), so this shape's behaviour is UNCHANGED by
// this fix. (The MUTATED analogue of this control is issue #4240 — a
// separate, pre-existing defect this fix does not reach either way; the
// read-only shape here is unaffected by that defect, which is why it is the
// correct control for THIS fix rather than the mutated one.)
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_returned_uaf_two_level_4226,
           read_only_two_level_nested_capture_not_returned_stays_crash_free_spec_28_9) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method One(): int "
        "    var f: () -> int := lambda() => 7; "
        "    var outer := lambda() => do "
        "      var inner := lambda() => do "
        "        var x := f(); "
        "      end; "
        "      inner(); "
        "    end; "
        "    outer(); "
        "    return f(); "
        "  end One; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var total := 0; "
        "    while i < 9 do "
        "      total := total + C.One(); "
        "      i := i + 1; "
        "    end; "
        "    if total <> 63 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// (E) BASELINE CONTROL — a closure-typed local returned by name with NO
// capturing lambda anywhere ("the only reason a plain, uncaptured closure
// return works at all", per #4198's own commit). Untouched by any of this
// machinery; included so the two-level fix's boundary is pinned by a test,
// not just by reasoning.
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_returned_uaf_two_level_4226,
           plain_uncaptured_closure_returned_unaffected_spec_27_5) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Plain(): () -> int "
        "    var f: () -> int := lambda() => 5; "
        "    return f; "
        "  end Plain; "
        "  public static method Main(): int "
        "    var g := C.Plain(); "
        "    return g(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}
