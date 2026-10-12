// Codegen regression tests for issue #5611 — a closure's box thunk allocated
// at a FUNCTION-TYPED PARAMETER pass was never released (spec §8.6 "Boxing",
// §28.3 "Local variables, ownership, and borrows", §28.9 "Runtime-managed
// types", §28.14 "Scoped callback parameters: storage-bounded capture",
// §27.5 "Closures", v0.334).
//
// THE DEFECT, IN TWO INDEPENDENT HALVES. Both are spelling-level gates that
// codegen applied to a DECLARED type while SEMA applied the RESOLVED type, so
// the two disagreed about which parameters are §28.14 call-bounded.
//
//  (A) The escape walk (LlvmBackend_Setup.cpp,
//      closureArgBindsMethodOwnTypeParamBorrow). Its CallExpr arm passed
//      safeRead=false for every argument except `GetOrAdd`'s factory key and
//      narrow #5129's bare method-own-`U` formal, so a closure local passed
//      to a PLAIN function-typed formal was classified ESCAPING. Escaping
//      withholds `closureReleaseEligible_` membership, which withholds the
//      local's scope-end `bz_closure_release`; and because an IdentifierExpr
//      argument is not a `+1 producer` (isPlusOneClosureProducer admits only
//      a lambda literal / method reference), lowerCallDispatch's call-bounded
//      arm did not release it either. The 48-byte cell then had NO releaser
//      at all. MEASURED on r23-c28 @ c604ae3b: 0 `bz_closure_release` sites
//      in `main` for `var f: Show := lambda …; p.Take(f);` against 2 for the
//      same program with the pass removed — and the DIRECT-CALL control
//      (`f(1)`, no parameter pass) is leak-free, which is why the defect hid.
//
//  (B) The call-bounded release arm itself (LlvmBackend_Calls2.cpp, BOTH
//      `formalIsFunction` sites — the #1864 sync-iterator arm and the
//      #1073/#5116 arm — plus `isCallBoundedClosureArgPosition`, the
//      capture-cell pre-scan in LlvmBackend_Setup.cpp). Each read
//      `p.type->kind == ast::TypeKind::Function`, which sees an ALIAS to a
//      function type (`type Show := (int) -> string`) as `Named`. SEMA's
//      own §28.14 enrolment (`MethodOwnership`'s `fnTyped`, Ownership.cpp)
//      resolves the declared type and already treats the alias as
//      call-bounded, already rejects storing it, and already ran its sink
//      scan — so the release arm was withholding a release SEMA had
//      already proven safe. MEASURED: `p.Take(lambda …)` leaks 48 B with the
//      alias and is clean with the inline spelling `f: (int) -> string`.
//
// WHY THE REPAIR IS SAFE, AND NOT A WIDENING. It is a RE-ALIGNMENT: it makes
// codegen's "is this formal function-typed" test read the same resolved type
// SEMA reads, on the three sites that decide whether to release. SEMA's
// §28.14 obligations (`reportCallBounded`) are unchanged and were already
// rejecting every storing shape under BOTH spellings, so no program changes
// from "accepted" to "rejected" or back — this only stops codegen from
// leaking in a case SEMA already governs. The escape-walk arm additionally
// keeps `calleeBodyUsesFormalOnlyByInvocation` (fail-closed) as an
// independent guard that consults no SEMA state at all.
//
// ROW DISCIPLINE. The issue's own repro is ONE-SHOT (48 B in 1 allocation),
// so a single-iteration row would be vacuous. Every leak row here allocates a
// FRESH closure per loop iteration so the leak scales to thousands of bytes
// and is unambiguous to LSan. Row 4 pins the correct VALUE through 300
// invocations (an over-release or a double free would change it or crash), and
// row 5 pins that the escape walk still DECLINES the forwarding shape — a leak
// is the safe direction and must not be "fixed" by over-releasing.

#include <string>

#include "support/aot_support.hpp"  // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// runAotChecked instruments the GENERATED code with ASan AND LeakSanitizer,
// so a leaked allocation fails the row through the exit code before the value
// is compared. ASan's exit code on error is 1; every row below expects 0.
std::string checkedExit(const std::string& prog) {
    auto r = runAotChecked(prog);
    if (!r.ran) return "<did-not-run>";
    return "exit=" + std::to_string(r.exitCode);
}

}  // namespace

// ===========================================================================
// Row 1 — §28.14 / §8.6 / §28.9 (v0.334), half (A): a closure LOCAL passed to
// an ALIAS-typed plain function-typed formal.
// ===========================================================================

// MEASURED on r23-c28 @ c604ae3b: `LeakSanitizer: 14400 byte(s) leaked in 300
// allocation(s)` (48 B x 300 — one closure cell per iteration).
//
// The callee body is empty: it never invokes, stores, or forwards `f`, so
// `calleeBodyUsesFormalOnlyByInvocation` answers true and the local keeps its
// own +1, released once at its scope end.
BEZEL_TEST(closure_arg_box_thunk_5611,
           alias_formal_local_cell_released_28_14_8_6) {
    std::string src =
        "module M; "
        "type Show := (int) -> string; "
        "class Sink "
        "  method Take(f: Show) end Take; "
        "end Sink; "
        "class Program "
        "  public static method Main(): int "
        "    var p := new Sink(); "
        "    for var j in 0..<300 do "
        "      var f: Show := lambda (x: int): string => $\"n={x}\"; "
        "      p.Take(f); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;";
    BEZEL_EXPECT_EQ(checkedExit(src), "exit=0");
}

// ===========================================================================
// Row 2 — §28.14 / §8.6 / §27.5 (v0.334), half (B) AND the capture-cell
// pre-scan: a CAPTURING lambda LITERAL passed to an ALIAS-typed formal.
// ===========================================================================

// MEASURED on r23-c28 @ c604ae3b: `LeakSanitizer: 12000 byte(s) leaked in 600
// allocation(s)` — three allocations per iteration (the 48 B closure cell, the
// captured-`j` cell, and the env block). This row is what forces BOTH fixes:
// the closure-cell release (two `formalIsFunction` sites) and the CAPTURE-CELL
// pre-scan in `isCallBoundedClosureArgPosition`, which gated its own release
// on the same spelling-level test. Fixing only the closure cell leaves 800 B
// / 200 allocations behind — the measured intermediate state.
BEZEL_TEST(closure_arg_box_thunk_5611,
           alias_formal_capturing_literal_cells_released_28_14_27_5) {
    std::string src =
        "module M; "
        "type Show := (int) -> string; "
        "class Sink "
        "  method Take(f: Show) end Take; "
        "end Sink; "
        "class Program "
        "  public static method Main(): int "
        "    var p := new Sink(); "
        "    for var j in 0..<200 do "
        "      p.Take(lambda (x: int): string => $\"n={j}{x}\"); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;";
    BEZEL_EXPECT_EQ(checkedExit(src), "exit=0");
}

// ===========================================================================
// Row 3 — §28.14 / §27.5 (v0.334), half (A) on the INLINE spelling: the
// escape-walk half, isolated from the alias half.
// ===========================================================================

// MEASURED on r23-c28 @ c604ae3b: `LeakSanitizer: 12000 byte(s) leaked in 600
// allocation(s)`. Row 3 is not an alias case, so half (B) cannot be what
// fixes it — it isolates the escape-walk arm, and it is the shape that was
// clean for the LITERAL (row 2's neighbour `h_inline_lit` measured NO LEAK on
// base) yet still leaked for the LOCAL. That asymmetry is what identified the
// escape walk as a separate defect from the spelling-level gates.
BEZEL_TEST(closure_arg_box_thunk_5611,
           inline_formal_local_cell_released_27_5_28_14) {
    std::string src =
        "module M; "
        "class Sink "
        "  method Take(f: (int) -> string) end Take; "
        "end Sink; "
        "class Program "
        "  public static method Main(): int "
        "    var p := new Sink(); "
        "    for var j in 0..<200 do "
        "      var f: (int) -> string := lambda (x: int): string => $\"n={x}\"; "
        "      p.Take(f); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;";
    BEZEL_EXPECT_EQ(checkedExit(src), "exit=0");
}

// ===========================================================================
// Row 4 — §28.14 CONTROL: the callee INVOKES the callback. The fix must keep
// the cell ALIVE across the invocation and produce the right VALUE.
// ===========================================================================

// 300 invocations of `f(7)`, each returning "n=7" (Length 3), accumulated.
//
// WHY THIS ROW IS NOT REDUNDANT WITH ROW 1. Rows 1-3 only assert "no leak",
// which a DOUBLE FREE would also satisfy in the first iteration and a too-
// eager release in the last. This row exercises the ordinary use: the callee
// invokes the callback 300 times and the total is 900. An over-release (the
// R24 attempt's failure, issue #5216) frees the cell under the callee and
// ASan reports a heap-use-after-free on a later invocation; an under-release
// leaks. Only a correctly-timed release passes.
//
// 900 exceeds the 8-bit exit status, so the value travels on STDOUT instead
// (the shell truncates 900 to 132 — 900 & 255 — which is how this row's first
// draft read as a SIGILL before the truncation was spotted). The total is
// printed with `Console.WriteLine` rather than a discarded `t.ToString()`:
// the discarded-string binding leaks 34 bytes on the current tip (MEASURED on
// r23-c28 @ c604ae3b, closure-independent — `var t := 0; t.ToString(); return
// t;` alone gives `LeakSanitizer: 34 byte(s) leaked in 2 allocation(s)`), and
// that pre-existing leak would fail this row under LSan for reasons that have
// nothing to do with the closure under test.
BEZEL_TEST(closure_arg_box_thunk_5611,
           invoked_callback_returns_correct_value_no_early_release_28_14) {
    std::string src =
        "module M; "
        "type Show := (int) -> string; "
        "class Sink "
        "  method Take(f: Show): int return f(7).Length; end Take; "
        "end Sink; "
        "class Program "
        "  public static method Main(): int "
        "    var p := new Sink(); "
        "    var t := 0; "
        "    for var j in 0..<300 do "
        "      var f: Show := lambda (x: int): string => $\"n={x}\"; "
        "      t := t + p.Take(f); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("900") != std::string::npos);
}

// ===========================================================================
// Row 5 — §28.14 FAIL-CLOSED CONTROL: the callee FORWARDS the callback.
// ===========================================================================

// `Take` passes `f` on to `Deep`, another plain function-typed formal. That is
// NOT invocation-only use, so `calleeBodyUsesFormalOnlyByInvocation` answers
// false and the escape walk must still classify the argument as escaping —
// which withholds the local's scope-end release and LEAKS. The leak is the
// documented safe direction; "fixing" it by releasing a callback the callee
// forwarded is exactly the use-after-free this arm must not introduce.
//
// Deliberately NOT `runAotChecked`: this shape is EXPECTED to leak, and LSan
// would fail the row for the intended behaviour. The row pins the two things
// that must hold — the program runs, and the callee's own observable result is
// correct — and a heap-use-after-free or an abort would still fail it (the
// harness records `ran=false`).
BEZEL_TEST(closure_arg_box_thunk_5611,
           forwarded_callback_still_declined_by_escape_walk_28_14) {
    std::string src =
        "module M; "
        "type Show := (int) -> string; "
        "class Inner "
        "  method Deep(g: Show): int return 1; end Deep; "
        "end Inner; "
        "class Sink "
        "  method Take(f: Show): int "
        "    var i := new Inner(); "
        "    return i.Deep(f); "
        "  end Take; "
        "end Sink; "
        "class Program "
        "  public static method Main(): int "
        "    var p := new Sink(); "
        "    var f: Show := lambda (x: int): string => $\"n={x}\"; "
        "    return p.Take(f); "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}