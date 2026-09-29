// Codegen — issue #1878 / spec §28.4 "The `destructor`", docs revision v0.306.
//
// §28.4 (v0.306), the throwing-destructor paragraph:
//   "**Destructors should not throw; behavior is defined when one does.** ...
//    - The exception propagates from the **disposal point** — the explicit
//      `Dispose()` call, the `using var`/auto-drop scope-end site, the
//      drop-on-overwrite site (§28.7), the owning container's cascade, or an
//      unbound owned temporary's end-of-full-expression (or unwind) disposal
//      (§28.3).
//    - Before it propagates, the **remaining steps of the generated
//      `Dispose()` still run** ... and physical reclamation still occurs — a
//      cleanup fault never leaks the object."
//
// VERIFIED ON HEAD 7f29b8ce (the issue's own repro, compiled + run):
//   dtor 2 / no-throw / after, exit 0
// — the destructor body ran and its `throw` was parked in the runtime's
// in-flight-exception slot by the generated `DisposeCore` (#1546's
// `dtor.threw`/`dtor.restore` tail), but the *disposal point* — the explicit
// `r.Dispose()` call site — emitted no pending check after the dispatch. Every
// other user-code call in this backend is followed by one; this one was not, so
// control fell straight through to `Console.WriteLine("no-throw")` and the
// enclosing `catch Exception` never ran. §28.4 requires "dtor 2 / caught /
// after".
//
// ROOT CAUSE / FIX: `Emitter::emitExplicitDisposeChain`
// (src/codegen/LlvmBackend_Calls2.cpp) now calls `emitDisposalFaultCheck` once,
// after the whole dispatch-arm chain, so the exception propagates from the
// disposal point while the reclaim tail (identity-hash reap, CauseChain
// reclaim, `free`, slot-null) has already run — §28.4's "physical reclamation
// still occurs".
//
// The check is BRACKETED, not a plain "is anything in flight": it snapshots the
// bz_exc slot before the dispatch (`snapshotInFlightExc`) and fires only when
// the slot ends up non-null AND different. A disposal site is one of the few
// places reached with somebody else's exception already in the slot — a
// destructor body on an unwind edge (#1468), an `x.Dispose()` inside an
// iterator `finally` running under §29.7 abandonment — and a plain non-null
// test misroutes those. What the comparison guarantees, and the one case it
// does not cover, is written out at `emitDisposalFaultCheck`'s definition.
//
// FAILS WITHOUT / PASSES WITH: every row below except
// `non_throwing_destructor_still_falls_through` (the no-spurious-unwind
// control) flips — confirmed by reverting the one-call hunk in
// LlvmBackend_Calls2.cpp, rebuilding, and re-running this filter.
//
// The `using var`/auto-drop scope-end site named in the same §28.4 bullet is
// the sibling defect #1882 and is pinned separately.

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// §33.3: the uncaught-exception boundary prints the type + message to stderr
// and exits 70 (EX_SOFTWARE).
constexpr int kUnhandledExceptionExit = 70;

// The issue's own class: a destructor that announces itself and then throws.
const char* kRes =
    "class Res "
    "  public id: int; "
    "  public constructor(x: int) id := x; end constructor; "
    "  destructor "
    "    Console.WriteLine(\"dtor \" + id.ToString()); "
    "    throw new Exception(\"cleanup-failed\"); "
    "  end destructor; "
    "end Res; ";

std::string program(const std::string& mainBody) {
    return std::string("module M; ") + kRes +
           "class Program "
           "  public static method Main(): int " +
           mainBody +
           "  end Main; "
           "end Program;";
}

}  // namespace

// §28.4 — THE ISSUE'S VERBATIM REPRO. The observable is the full stdout
// TRANSCRIPT, not merely a non-zero exit: it pins (a) that the destructor body
// ran ("dtor 2"), (b) that the statement after the disposal point did NOT run
// (no "no-throw" line — the pre-fix fall-through), (c) that the *enclosing*
// `catch Exception` arm is what ran ("caught"), and (d) that control resumed
// after the `try` ("after") with a normal exit.
BEZEL_TEST(throwing_destructor_propagates_from_explicit_dispose_1878,
           explicit_dispose_propagates_to_enclosing_catch_spec_28_4) {
    auto r = runAot(program(
        "    var r := new Res(2); "
        "    try "
        "      r.Dispose(); "
        "      Console.WriteLine(\"no-throw\"); "
        "    catch Exception => Console.WriteLine(\"caught\") "
        "    end; "
        "    Console.WriteLine(\"after\"); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("dtor 2\ncaught\nafter\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.4 — WHAT propagates is the destructor's OWN exception instance, not some
// substitute the disposal path minted: the arm binds it and prints its
// `Message`, which must be the destructor's "cleanup-failed".
BEZEL_TEST(throwing_destructor_propagates_from_explicit_dispose_1878,
           propagated_instance_is_the_destructors_own_exception_spec_28_4) {
    auto r = runAot(program(
        "    var r := new Res(2); "
        "    try "
        "      r.Dispose(); "
        "      Console.WriteLine(\"no-throw\"); "
        "    catch var ex: Exception => Console.WriteLine(ex.Message) "
        "    end; "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("dtor 2\ncleanup-failed\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.4 + §31.2 — it is real propagation through the handler chain, not a
// blanket jump to the nearest `try`: an inner arm whose type does NOT match
// (`ArgumentException`) must decline, and the OUTER `Exception` arm must
// receive it. Pre-fix the whole thing was swallowed and `Main` returned 1.
BEZEL_TEST(throwing_destructor_propagates_from_explicit_dispose_1878,
           non_matching_inner_arm_declines_outer_arm_receives_spec_31_2) {
    auto r = runAot(program(
        "    var r := new Res(2); "
        "    try "
        "      try "
        "        r.Dispose(); "
        "        return 1; "
        "      catch ArgumentException => return 2; "
        "      end; "
        "    catch Exception => return 3; "
        "    end; "
        "    return 4; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// §28.4 + §33.3 — with NO handler anywhere the exception must propagate out of
// `Main` and hit the uncaught-exception boundary (exit 70). Pre-fix it was
// swallowed and the program fell through to `return 55` printing "after"; the
// stdout assertion is what distinguishes the two, since a bare "exit != 0"
// oracle would pass on the pre-fix `return 55` too.
BEZEL_TEST(throwing_destructor_propagates_from_explicit_dispose_1878,
           unhandled_disposal_fault_reaches_the_process_boundary_spec_33_3) {
    auto r = runAot(program(
        "    var r := new Res(2); "
        "    r.Dispose(); "
        "    Console.WriteLine(\"after\"); "
        "    return 55; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kUnhandledExceptionExit);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("dtor 2\n"));
}

// §28.4 — CONTROL (unchanged by the fix): a destructor that does NOT throw
// leaves nothing in flight, so the statement after the disposal point still
// runs. Pins that the added pending check is a no-op on the ordinary path and
// does not manufacture a spurious unwind.
BEZEL_TEST(throwing_destructor_propagates_from_explicit_dispose_1878,
           non_throwing_destructor_still_falls_through) {
    auto r = runAot(
        "module M; "
        "class Quiet "
        "  destructor Console.WriteLine(\"dtor\"); end destructor; "
        "end Quiet; "
        "class Program "
        "  public static method Main(): int "
        "    var q := new Quiet(); "
        "    try "
        "      q.Dispose(); "
        "      Console.WriteLine(\"no-throw\"); "
        "    catch Exception => Console.WriteLine(\"caught\") "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("dtor\nno-throw\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.4 — "Before it propagates, the remaining steps of the generated
// `Dispose()` still run ... and physical reclamation still occurs — a cleanup
// fault never leaks the object." The owned field's destructor must have run
// (encoded in the exit code) while the fault travels from the disposal point to
// the handler, and the program must stay ASan/LSan-clean doing it.
//
// ⚠ The EXIT CODE is what flips here; do not read `r.ran` as this row's leak
// oracle. The pre-fix compiler is ASan-clean on this program too — the
// swallowed exception stays reachable from the global in-flight slot, so LSan
// does not flag it — and a single iteration is exactly the shape this project's
// own lesson calls vacuous for leak claims. `runAotChecked` is kept because it
// is free here and would catch a double-free or UAF introduced later, but the
// contract this row pins is the behavioural one: handler ran AND owned field
// disposed. The leak-shaped claims that ARE load-bearing live in the two
// `..._does_not_leak_the_frame_spec_29_7` rows of the #1882 file, where the
// three-way measurement is written out.
BEZEL_TEST(throwing_destructor_propagates_from_explicit_dispose_1878,
           remaining_dispose_steps_run_and_nothing_leaks_spec_28_4) {
    auto r = runAotChecked(
        "module M; "
        "class Field "
        "  public static _n: int := 0; "
        "  destructor Field._n := Field._n + 1; end destructor; "
        "end Field; "
        "class Owner "
        "  private owned _f: Field; "
        "  public constructor(owned f: Field) _f := f; end constructor; "
        "  destructor throw new Exception(\"cleanup-failed\"); end destructor; "
        "end Owner; "
        "class Program "
        "  public static method Main(): int "
        "    var f := new Field(); "
        "    var o := new Owner(owned f); "
        "    try "
        "      o.Dispose(); "
        "      return 1; "                       // pre-fix: swallowed, returns 1
        "    catch Exception => return 10 + Field._n; "
        "    end; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);          // no double-free / UAF (NOT a leak claim)
    BEZEL_EXPECT_EQ(r.exitCode, 11);   // handler ran AND the owned field was disposed
}
