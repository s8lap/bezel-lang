// Regression tests for #2881 — a catch-arm `when` guard that THROWS skipped its
// own try's `finally` entirely.
//
// WHAT THE DOCUMENT REQUIRES.  Spec §31.2 "`try`/`catch`/`finally`" (v0.240):
//
//     `finally` runs whether the protected block completed normally, was exited
//     by an exception (caught or uncaught), or was exited by `return` — or by a
//     `break` or `continue` targeting an enclosing loop.
//
// and §31.2.1 "Arm patterns" (v0.240), on the guard:
//
//     If the guard itself **throws**, that new exception *supersedes* the
//     in-flight one — the superseded exception is **disposed by the runtime
//     exactly once**, as if its handling had completed (the two owner-owned
//     instances excepted, §31.2) — and propagates to the enclosing handler — it
//     is not caught by a later arm of the same `try` and does not resume guard
//     evaluation.
//
// (Both block quotes reproduce the spec text verbatim except that its inline
// markdown link targets — `[§31.2](#sec-try-catch-finally)` — are shown as bare
// `§31.2`.)
//
// Those two sentences settle the whole shape, including the question the issue
// flagged as unsettled — WHICH exception leaves the `try`.  The protected block
// "was exited by an exception", and the guard's NEW exception is the one that
// "propagates to the enclosing handler", so: the `finally` must run, exactly
// once, on that edge; the superseding (guard's) exception is what the enclosing
// handler sees; and the superseded in-flight exception is disposed exactly once.
// The `finally`'s own supersede rule composes on top (§31.2): if the `finally`
// entered on that edge itself throws, ITS exception replaces the guard's.
//
// THE DEFECT.  `lowerTry` (src/codegen/LlvmBackend_Stmts.cpp) restores
// `unwindTarget_` to the ENCLOSING handler right after the try body, and only
// re-points it at this try's `finUnwindBB` inside `armBB` — i.e. after the guard
// has already selected the arm.  The guard was therefore lowered while the unwind
// target was still the enclosing handler, so a `throw` from the guard (a literal
// `throw`, a call that propagates, or a `new` whose constructor throws) branched
// straight past this try's `finally` to the outer handler.
//
// MEASURED pre-fix (each program below, exit code = a failure bitmask, 0 = pass):
//   the issue's repro returned 0 where §31.2 requires 1 (the `finally` never
//   ran); every guard-throw shape here — call-propagated, `throw`-expression,
//   throwing constructor, second arm, after a false guard, async, await-in-
//   finally, nested try, enclosing `using var`, body `using var`, in-loop —
//   showed the same skipped `finally`.  Unaffected pre-fix and after: a
//   non-throwing guard (true or false), a guard throw in a try with NO `finally`,
//   an unmatched arm, and an arm body that re-raises.
//
// THE FIX installs this try's `finUnwindBB` as `unwindTarget_` (with the matching
// `unwindScopeDepth_`) for the duration of the GUARD as well as the arm body.
// The disposal set on that edge is unchanged, only RELOCATED: pre-fix the guard's
// throw site disposed [savedDepth, thisTryDepth) itself; post-fix it stops at
// thisTryDepth and finUnwindBB's `emitUnwindDisposalsInRange(savedDepth,
// thisTryDepth)` disposes exactly that range AFTER the finally body — the §28.3
// finally-first ordering.  The superseded exception's single free is unchanged
// too: it is performed at the guard's own throw / pending-check site by the
// #1108 / #1216 path, which fires iff `caughtExcArmTarget_` equals the throw's
// resolved unwind target — both are derived from `unwindTarget_`, so they still
// compare equal once both are `finUnwindBB`.
//
// THE ORACLE IS A LIVE-OBJECT COUNTER, NOT "NO LEAK".  The direction this fix
// can break is a DOUBLE dispose (the relocated range disposed at both the throw
// site and in finUnwindBB) or a DOUBLE `finally` run, and LeakSanitizer sees
// neither.  Every program folds a live count AND a destructor count AND the
// `finally` count into the exit code, runs under generated-code ASan+LSan, and
// amplifies its loop to 20 iterations.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// ASan + LSan on the generated program: a leak, double-free or UAF surfaces as
// ran=false (-999); otherwise the program's exit code.
int run(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// Plain AOT — for the ordering oracle, whose observable is a sequence value.
int runPlain(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// `GX` is the exception the GUARD raises, `BX` the one the try BODY raises, `FX`
// the one a `finally` raises, `ZX` an unrelated one.  Distinct classes so the
// enclosing handler can assert BY TYPE which exception actually propagated —
// §31.2.1 says it must be the guard's (the superseding one), not the body's.
// `Src` counts live instances AND destructor runs, so the oracle distinguishes a
// leak (live > 0) from a double dispose (dtor > created).  `Bad`'s constructor
// throws, giving the emitNewPendingCheck flavour of a guard throw.
const char* kPrelude =
    "module M; "
    "class GX end GX; "
    "class BX end BX; "
    "class FX end FX; "
    "class ZX end ZX; "
    "class Src "
    "  public static _live: int := 0; "
    "  public static _dtor: int := 0; "
    "  private _v: int := 5; "
    "  public constructor() Src._live := Src._live + 1; end constructor; "
    "  destructor "
    "    Src._live := Src._live - 1; "
    "    Src._dtor := Src._dtor + 1; "
    "  end destructor; "
    "  public method V(): int return _v; end V; "
    "end Src; "
    "class Bad "
    "  public constructor() throw new GX(); end constructor; "
    "  public method V(): int return 1; end V; "
    "end Bad; "
    "class T "
    "  public static method BodyBoom(): int throw new BX(); end BodyBoom; "
    "  public static method GuardBoom(): int throw new GX(); end GuardBoom; "
    "  public static method No(): int return 0; end No; "
    "end T; ";

// A program whose `Main` runs `mainBody` (which folds failures into `bad`).
std::string program(const std::string& members, const std::string& mainBody) {
    return std::string(kPrelude) +
           "class C "
           "  public static _fin: int := 0; "
           "  public static _got: int := 0; " +
           members +
           "  public static method Main(): int "
           "    var bad := 0; "
           "    Src._live := 0; Src._dtor := 0; C._fin := 0; C._got := 0; " +
           mainBody +
           "    return bad; "
           "  end Main; "
           "end C;";
}

// The async twin (Main must itself be async to await the callee).
std::string asyncProgram(const std::string& members,
                         const std::string& mainBody) {
    return std::string(kPrelude) +
           "class C "
           "  public static _fin: int := 0; "
           "  public static _got: int := 0; " +
           members +
           "  public static async method Main(): Task<int> "
           "    var bad := 0; "
           "    Src._live := 0; Src._dtor := 0; C._fin := 0; C._got := 0; " +
           mainBody +
           "    return bad; "
           "  end Main; "
           "end C;";
}

// Twenty calls of `C.Once()`, each expected to leave by the guard's GX. `extra`
// adds further post-loop assertions.
std::string callLoop(const std::string& extra) {
    return
        "    var i := 0; "
        "    while i < 20 do "
        "      try "
        "        var r := C.Once(); "
        "        C._got := C._got + 10000; "     // Once must never return
        "      catch GX => C._got := C._got + 1; "
        "      catch BX => C._got := C._got + 100; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 20 then bad := bad + 1; end if; "
        "    if C._got <> 20 then bad := bad + 2; end if; " + extra;
}

const char* kCounters =
    "    if Src._live <> 0 then bad := bad + 4; end if; ";

}  // namespace

// ===========================================================================
// FAIL-WITHOUT / PASS-WITH
// ===========================================================================

// §31.2 (v0.240) — the issue's own repro, verbatim in shape: the `finally` must
// have run when the guard's exception reaches the enclosing handler. Pre-fix
// this program returned 0 (`C._fin` never incremented).
BEZEL_TEST(finally_throwing_guard_2881,
           guard_throw_runs_this_trys_finally_spec_31_2) {
    BEZEL_EXPECT_EQ(run(std::string(kPrelude) +
        "class C "
        "  public static _fin: int := 0; "
        "  public static method Once(): int "
        "    try "
        "      throw new BX(); "
        "    catch var e: BX when T.GuardBoom() = 1 => "
        "      return 1; "
        "    finally "
        "      C._fin := C._fin + 1; "
        "    end; "
        "    return 2; "
        "  end Once; "
        "  public static method Main(): int "
        "    C._fin := 0; "
        "    var caught := 0; "
        "    try "
        "      var r := C.Once(); "
        "      if r <> 0 then return 90; end if; "
        "    catch var e2: GX => "
        "      caught := 1; "
        "    end; "
        "    if caught <> 1 then return 91; end if; "
        "    return C._fin; "                    // §31.2: 1. Pre-fix: 0.
        "  end Main; "
        "end C;"), 1);
}

// §31.2.1 (v0.240) — the exception the enclosing handler receives is the GUARD's
// (the superseding one), not the body's, and it arrives AFTER the `finally` ran.
// Asserted by TYPE, so a fix that routed the wrong object through finUnwindBB
// would fail here even though the `finally` count is right.
BEZEL_TEST(finally_throwing_guard_2881,
           superseding_guard_exception_is_what_propagates_spec_31_2_1) {
    BEZEL_EXPECT_EQ(run(program(
        "  public static method Once(): int "
        "    try "
        "      throw new BX(); "
        "    catch var e: BX when T.GuardBoom() = 1 => "
        "      C._got := C._got + 1000; "
        "    finally "
        "      C._fin := C._fin + 1; "
        "    end; "
        "    return 0; "
        "  end Once; ",
        callLoop(""))), 0);
}

// §31.2 / §28.3 — an owned local live in the try body when the guard throws is
// disposed EXACTLY ONCE (live == 0, dtor == created) and the `finally` runs
// exactly once per call. This is the shape where the fix RELOCATES the disposal
// from the throw site into finUnwindBB, so a double dispose would show here.
BEZEL_TEST(finally_throwing_guard_2881,
           body_owned_local_disposed_exactly_once_spec_28_3) {
    BEZEL_EXPECT_EQ(run(program(
        "  public static method Once(): int "
        "    try "
        "      var s := new Src(); "
        "      var k := s.V(); "
        "      k := k + T.BodyBoom(); "
        "      s.Dispose(); "
        "    catch var e: BX when T.GuardBoom() = 1 => "
        "      C._got := C._got + 1000; "
        "    finally "
        "      C._fin := C._fin + 1; "
        "    end; "
        "    return 0; "
        "  end Once; ",
        callLoop(std::string(kCounters) +
                 "    if Src._dtor <> 20 then bad := bad + 8; end if; "))), 0);
}

// EDGE CONSISTENCY (not a normative ordering claim). The guard-throw edge must
// interleave the `finally` body and the try body's implicit disposal exactly as
// the try's OTHER exception edges do — both now route through the same
// `finUnwindBB`, which runs the finally body and then
// `emitUnwindDisposalsInRange`. The oracle compares the guard-throw sequence
// against the unmatched-arm sequence of the identical try (1 = finally body,
// 2 = the body local's destructor), so a future change to that shared
// interleave moves both together or fails here. Pre-fix the guard edge produced
// 2 (the finally never ran) against the unmatched edge's 12.
//
// (The tree's interleave for BOTH edges is finally-then-dispose, pinned since
// #1263 by finally_before_autodispose_1263. Whether that is what §28.3/§28.10's
// reverse-establishment rule requires for a `finally` established at try entry
// and a local declared inside the body is a separate, pre-existing question —
// filed, not decided here. This test deliberately asserts only that the two
// edges agree, plus the value the tree currently produces.)
BEZEL_TEST(finally_throwing_guard_2881,
           guard_throw_edge_interleaves_like_the_unmatched_edge_2881) {
    const char* kSeqPrelude =
        "module M; "
        "class GX end GX; "
        "class BX end BX; "
        "class T "
        "  public static method BodyBoom(): int throw new BX(); end BodyBoom; "
        "  public static method GuardBoom(): int throw new GX(); end GuardBoom; "
        "end T; "
        "class Seq "
        "  public static _seq: int := 0; "
        "  private _v: int := 5; "
        "  public method V(): int return _v; end V; "
        "  destructor Seq._seq := Seq._seq * 10 + 2; end destructor; "
        "end Seq; ";
    // Edge A: the arm's pattern matches and its guard THROWS.
    BEZEL_EXPECT_EQ(runPlain(std::string(kSeqPrelude) +
        "class C "
        "  public static method Once(): int "
        "    try "
        "      var s := new Seq(); "
        "      var k := s.V(); "
        "      k := k + T.BodyBoom(); "
        "      s.Dispose(); "
        "    catch var e: BX when T.GuardBoom() = 1 => "
        "      Seq._seq := Seq._seq * 10 + 9; "
        "    finally "
        "      Seq._seq := Seq._seq * 10 + 1; "
        "    end; "
        "    return 0; "
        "  end Once; "
        "  public static method Main(): int "
        "    Seq._seq := 0; "
        "    try "
        "      var r := C.Once(); "
        "    catch GX => Seq._seq := Seq._seq + 0; "
        "    end; "
        "    return Seq._seq; "
        "  end Main; "
        "end C;"), 12);
    // Edge B: no arm matches at all — the reference interleave, unchanged by
    // this fix and green before it.
    BEZEL_EXPECT_EQ(runPlain(std::string(kSeqPrelude) +
        "class C "
        "  public static method Once(): int "
        "    try "
        "      var s := new Seq(); "
        "      var k := s.V(); "
        "      k := k + T.BodyBoom(); "
        "      s.Dispose(); "
        "    catch var e: GX => "
        "      Seq._seq := Seq._seq * 10 + 9; "
        "    finally "
        "      Seq._seq := Seq._seq * 10 + 1; "
        "    end; "
        "    return 0; "
        "  end Once; "
        "  public static method Main(): int "
        "    Seq._seq := 0; "
        "    try "
        "      var r := C.Once(); "
        "    catch BX => Seq._seq := Seq._seq + 0; "
        "    end; "
        "    return Seq._seq; "
        "  end Main; "
        "end C;"), 12);
}

// §31.2.1 (v0.240) — "it is not caught by a later arm of the same `try`": a
// LATER arm of this same try whose pattern would match the guard's exception
// must not run; the exception leaves the try (through the finally).
BEZEL_TEST(finally_throwing_guard_2881,
           guard_throw_not_caught_by_later_arm_of_same_try_spec_31_2_1) {
    BEZEL_EXPECT_EQ(run(program(
        "  public static method Once(): int "
        "    try "
        "      throw new BX(); "
        "    catch var e: BX when T.GuardBoom() = 1 => "
        "      C._got := C._got + 1000; "
        "    catch GX => "
        "      C._got := C._got + 500; "         // must NOT be selected
        "    finally "
        "      C._fin := C._fin + 1; "
        "    end; "
        "    return 0; "
        "  end Once; ",
        callLoop(""))), 0);
}

// §31.5 / §31.2.1 — the guard IS a `throw` expression (the `diverged_` path,
// #1097: the throw's own unwind branch terminates the prep block).
BEZEL_TEST(finally_throwing_guard_2881,
           guard_is_a_throw_expression_runs_finally_spec_31_5) {
    BEZEL_EXPECT_EQ(run(program(
        "  public static method Once(): int "
        "    try "
        "      throw new BX(); "
        "    catch BX when throw new GX() => "
        "      C._got := C._got + 1000; "
        "    finally "
        "      C._fin := C._fin + 1; "
        "    end; "
        "    return 0; "
        "  end Once; ",
        callLoop(""))), 0);
}

// §31.2.1 — a `new` inside the guard whose CONSTRUCTOR throws: the third of the
// three sites that can leave a guard (emitNewPendingCheck, alongside
// emitThrowValue and emitPendingCheck). Each frees the superseded exception, so
// each must also route through the finally.
BEZEL_TEST(finally_throwing_guard_2881,
           guard_constructor_throw_runs_finally_spec_31_2_1) {
    BEZEL_EXPECT_EQ(run(program(
        "  public static method Once(): int "
        "    try "
        "      throw new BX(); "
        "    catch var e: BX when (new Bad()).V() = 1 => "
        "      C._got := C._got + 1000; "
        "    finally "
        "      C._fin := C._fin + 1; "
        "    end; "
        "    return 0; "
        "  end Once; ",
        callLoop(""))), 0);
}

// §31.2 (v0.240) — the `finally` entered on the guard-throw edge itself throws.
// "when a `finally` entered during exception propagation itself completes by
// throwing ... the new exception replaces the one being handled": FX supersedes
// the guard's GX, which in turn superseded the body's BX. The enclosing handler
// must see FX, and neither superseded instance may leak or be freed twice.
BEZEL_TEST(finally_throwing_guard_2881,
           finally_throw_supersedes_the_guards_exception_spec_31_2) {
    BEZEL_EXPECT_EQ(run(program(
        "  public static method Once(): int "
        "    try "
        "      throw new BX(); "
        "    catch var e: BX when T.GuardBoom() = 1 => "
        "      C._got := C._got + 1000; "
        "    finally "
        "      C._fin := C._fin + 1; "
        "      throw new FX(); "
        "    end; "
        "    return 0; "
        "  end Once; ",
        "    var i := 0; "
        "    while i < 20 do "
        "      try "
        "        var r := C.Once(); "
        "        C._got := C._got + 10000; "
        "      catch FX => C._got := C._got + 1; "
        "      catch GX => C._got := C._got + 100; "
        "      catch BX => C._got := C._got + 1000; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 20 then bad := bad + 1; end if; "
        "    if C._got <> 20 then bad := bad + 2; end if; ")), 0);
}

// §31.2 / §28.3 — NESTED try: the inner try (finally + throwing guard) sits
// inside an outer try that also has a finally. BOTH finallys must run (40), and
// both owned locals disposed exactly once.
BEZEL_TEST(finally_throwing_guard_2881,
           nested_try_guard_throw_runs_both_finallys_spec_31_2) {
    BEZEL_EXPECT_EQ(run(program(
        "  public static method Once(): int "
        "    try "
        "      var outerv := new Src(); "
        "      try "
        "        var s := new Src(); "
        "        var k := s.V(); "
        "        k := k + T.BodyBoom(); "
        "        s.Dispose(); "
        "      catch var e: BX when T.GuardBoom() = 1 => "
        "        C._got := C._got + 1000; "
        "      finally "
        "        C._fin := C._fin + 1; "
        "      end; "
        "      outerv.Dispose(); "
        "    finally "
        "      C._fin := C._fin + 1; "
        "    end; "
        "    return 0; "
        "  end Once; ",
        "    var i := 0; "
        "    while i < 20 do "
        "      try "
        "        var r := C.Once(); "
        "        C._got := C._got + 10000; "
        "      catch GX => C._got := C._got + 1; "
        "      catch BX => C._got := C._got + 100; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 40 then bad := bad + 1; end if; "
        "    if C._got <> 20 then bad := bad + 2; end if; "
        "    if Src._live <> 0 then bad := bad + 4; end if; "
        "    if Src._dtor <> 40 then bad := bad + 8; end if; ")), 0);
}

// §28.3 (v0.240) — an enclosing `using var` established BEFORE the try lies in
// the range finUnwindBB re-propagates through
// (emitUnwindDisposalsInRange(savedDepth, thisTryDepth)). Pre-fix the guard's
// throw site disposed it; post-fix finUnwindBB does. Either way exactly once —
// this pins the relocation against a double dispose.
BEZEL_TEST(finally_throwing_guard_2881,
           enclosing_using_var_disposed_exactly_once_spec_28_3) {
    BEZEL_EXPECT_EQ(run(program(
        "  public static method Once(): int "
        "    using var outer := new Src(); "
        "    try "
        "      var s := new Src(); "
        "      var k := s.V(); "
        "      k := k + T.BodyBoom(); "
        "      s.Dispose(); "
        "    catch var e: BX when T.GuardBoom() = 1 => "
        "      C._got := C._got + 1000; "
        "    finally "
        "      C._fin := C._fin + 1; "
        "    end; "
        "    return 0; "
        "  end Once; ",
        callLoop(std::string(kCounters) +
                 "    if Src._dtor <> 40 then bad := bad + 8; end if; "))), 0);
}

// §28.3 / §28.10 — a `using var` in the try BODY on the guard-throw edge:
// §28.10 "Disposal happens on every exit path: normal completion, `break`,
// `return`, or exception."
BEZEL_TEST(finally_throwing_guard_2881,
           body_using_var_disposed_exactly_once_spec_28_10) {
    BEZEL_EXPECT_EQ(run(program(
        "  public static method Once(): int "
        "    try "
        "      using var s := new Src(); "
        "      var k := s.V(); "
        "      k := k + T.BodyBoom(); "
        "    catch var e: BX when T.GuardBoom() = 1 => "
        "      C._got := C._got + 1000; "
        "    finally "
        "      C._fin := C._fin + 1; "
        "    end; "
        "    return 0; "
        "  end Once; ",
        callLoop(std::string(kCounters) +
                 "    if Src._dtor <> 20 then bad := bad + 8; end if; "))), 0);
}

// §31.2.1 — the guard that throws is on the SECOND arm; the first arm's pattern
// does not match. The redirect must be installed per-arm, not once.
BEZEL_TEST(finally_throwing_guard_2881,
           second_arm_guard_throw_runs_finally_spec_31_2_1) {
    BEZEL_EXPECT_EQ(run(program(
        "  public static method Once(): int "
        "    try "
        "      throw new BX(); "
        "    catch ZX => C._got := C._got + 500; "
        "    catch var e: BX when T.GuardBoom() = 1 => "
        "      C._got := C._got + 1000; "
        "    finally "
        "      C._fin := C._fin + 1; "
        "    end; "
        "    return 0; "
        "  end Once; ",
        callLoop(""))), 0);
}

// §19.4 / §31.2.1 — a FALSE guard on the first arm, then a THROWING guard on the
// second: "if the guard returns `false`, the arm is skipped and the next arm is
// considered" and the next guard's throw still routes through the finally.
BEZEL_TEST(finally_throwing_guard_2881,
           guard_throw_after_a_false_guard_runs_finally_spec_19_4) {
    BEZEL_EXPECT_EQ(run(program(
        "  public static method Once(): int "
        "    try "
        "      throw new BX(); "
        "    catch var a: BX when T.No() = 1 => C._got := C._got + 500; "
        "    catch var b: BX when T.GuardBoom() = 1 => "
        "      C._got := C._got + 1000; "
        "    finally "
        "      C._fin := C._fin + 1; "
        "    end; "
        "    return 0; "
        "  end Once; ",
        callLoop(""))), 0);
}

// §30.9 / §31.2 — the same shape inside an ASYNC method's state machine, where
// the finally's exit-mode slots are frame-backed and the unwind path completes
// the task with the fault.
BEZEL_TEST(finally_throwing_guard_2881,
           async_guard_throw_runs_finally_spec_30_9) {
    BEZEL_EXPECT_EQ(run(asyncProgram(
        "  public static async method Once(): Task<int> "
        "    try "
        "      var s := new Src(); "
        "      var k := s.V(); "
        "      k := k + T.BodyBoom(); "
        "      s.Dispose(); "
        "    catch var e: BX when T.GuardBoom() = 1 => "
        "      C._got := C._got + 1000; "
        "    finally "
        "      C._fin := C._fin + 1; "
        "    end; "
        "    return 0; "
        "  end Once; ",
        "    var i := 0; "
        "    while i < 20 do "
        "      try "
        "        var r := await C.Once(); "
        "        C._got := C._got + 10000; "
        "      catch GX => C._got := C._got + 1; "
        "      catch BX => C._got := C._got + 100; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 20 then bad := bad + 1; end if; "
        "    if C._got <> 20 then bad := bad + 2; end if; "
        "    if Src._live <> 0 then bad := bad + 4; end if; "
        "    if Src._dtor <> 20 then bad := bad + 8; end if; ")), 0);
}

// §30.9 / §31.2 — the `finally` reached on the guard-throw edge itself contains
// an `await` (the finallyAwaits branch of finUnwindBB, which frame-backs the
// captured in-flight exception across the cross-thread resume). The guard's
// exception must still be the one restored and re-propagated afterwards.
BEZEL_TEST(finally_throwing_guard_2881,
           async_await_in_finally_on_guard_throw_edge_spec_30_9) {
    BEZEL_EXPECT_EQ(run(asyncProgram(
        "  public static async method Tick(): Task<int> return 1; end Tick; "
        "  public static async method Once(): Task<int> "
        "    try "
        "      throw new BX(); "
        "    catch var e: BX when T.GuardBoom() = 1 => "
        "      C._got := C._got + 1000; "
        "    finally "
        "      var t := await C.Tick(); "
        "      C._fin := C._fin + t; "
        "    end; "
        "    return 0; "
        "  end Once; ",
        "    var i := 0; "
        "    while i < 20 do "
        "      try "
        "        var r := await C.Once(); "
        "        C._got := C._got + 10000; "
        "      catch GX => C._got := C._got + 1; "
        "      catch BX => C._got := C._got + 100; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 20 then bad := bad + 1; end if; "
        "    if C._got <> 20 then bad := bad + 2; end if; ")), 0);
}

// §21.5 / §31.2 — the try sits inside a loop and a DIFFERENT arm `continue`s it
// (exit mode 3). The guard-throw edge must leave the exit-mode machinery alone:
// iterations 0-2 take the `continue` arm, iteration 3's guard throws and escapes
// the loop entirely. `_fin` = 4 (one per iteration entered), `_got` = 3
// continues + the escaping GX.
BEZEL_TEST(finally_throwing_guard_2881,
           guard_throw_leaves_other_arm_exit_modes_intact_spec_21_5) {
    BEZEL_EXPECT_EQ(run(program(
        "  public static method Once(): int "
        "    var i := 0; "
        "    while i < 20 do "
        "      try "
        "        var s := new Src(); "
        "        var k := s.V(); "
        "        if i = 3 then k := k + T.BodyBoom(); end if; "
        "        s.Dispose(); "
        "        throw new ZX(); "
        "      catch var e: BX when T.GuardBoom() = 1 => "
        "        C._got := C._got + 1000; "
        "      catch ZX => do "
        "        C._got := C._got + 1; "
        "        i := i + 1; "
        "        continue; "
        "      end "
        "      finally "
        "        C._fin := C._fin + 1; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Once; ",
        "    try "
        "      var r := C.Once(); "
        "      bad := bad + 16; "
        "    catch GX => C._got := C._got + 10000; "
        "    catch BX => C._got := C._got + 100; "
        "    end; "
        "    if C._fin <> 4 then bad := bad + 1; end if; "
        "    if C._got <> 10003 then bad := bad + 2; end if; "
        "    if Src._live <> 0 then bad := bad + 4; end if; "
        "    if Src._dtor <> 4 then bad := bad + 8; end if; ")), 0);
}

// ===========================================================================
// ACCEPT CONTROLS — green BOTH before and after. They exist to catch an
// over-wide redirect (a finally run twice, a disposal run twice, a guard's
// normal continuation misrouted), not to catch the bug.
// ===========================================================================

// §31.2.1 — a try with a throwing guard and NO `finally` is untouched: the
// guard's exception propagates and the body's owned local is disposed once.
BEZEL_TEST(finally_throwing_guard_2881,
           guard_throw_without_finally_unchanged_spec_31_2_1) {
    BEZEL_EXPECT_EQ(run(program(
        "  public static method Once(): int "
        "    try "
        "      var s := new Src(); "
        "      var k := s.V(); "
        "      k := k + T.BodyBoom(); "
        "      s.Dispose(); "
        "    catch var e: BX when T.GuardBoom() = 1 => "
        "      C._got := C._got + 1000; "
        "    end; "
        "    return 0; "
        "  end Once; ",
        "    var i := 0; "
        "    while i < 20 do "
        "      try "
        "        var r := C.Once(); "
        "        C._got := C._got + 10000; "
        "      catch GX => C._got := C._got + 1; "
        "      catch BX => C._got := C._got + 100; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 0 then bad := bad + 1; end if; "
        "    if C._got <> 20 then bad := bad + 2; end if; "
        "    if Src._live <> 0 then bad := bad + 4; end if; "
        "    if Src._dtor <> 20 then bad := bad + 8; end if; ")), 0);
}

// §19.4 — a NON-throwing guard that is true selects its arm; the finally runs
// exactly once per call (20, not 40).
BEZEL_TEST(finally_throwing_guard_2881,
           non_throwing_true_guard_selects_arm_spec_19_4) {
    BEZEL_EXPECT_EQ(run(program(
        "  public static method Once(): int "
        "    var n := 5; "
        "    try "
        "      var s := new Src(); "
        "      var k := s.V(); "
        "      k := k + T.BodyBoom(); "
        "      s.Dispose(); "
        "    catch BX when n > 3 => return 1; "
        "    catch BX => return 2; "
        "    finally "
        "      C._fin := C._fin + 1; "
        "    end; "
        "    return 3; "
        "  end Once; ",
        "    var i := 0; "
        "    while i < 20 do "
        "      if C.Once() <> 1 then bad := bad + 16; end if; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 20 then bad := bad + 1; end if; "
        "    if Src._live <> 0 then bad := bad + 4; end if; "
        "    if Src._dtor <> 20 then bad := bad + 8; end if; ")), 0);
}

// §19.4 — a NON-throwing guard that is false falls through to the next arm.
BEZEL_TEST(finally_throwing_guard_2881,
           non_throwing_false_guard_falls_to_next_arm_spec_19_4) {
    BEZEL_EXPECT_EQ(run(program(
        "  public static method Once(): int "
        "    var n := 1; "
        "    try "
        "      throw new BX(); "
        "    catch BX when n > 3 => return 1; "
        "    catch BX => return 2; "
        "    finally "
        "      C._fin := C._fin + 1; "
        "    end; "
        "    return 3; "
        "  end Once; ",
        "    var i := 0; "
        "    while i < 20 do "
        "      if C.Once() <> 2 then bad := bad + 16; end if; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 20 then bad := bad + 1; end if; ")), 0);
}

// §31.2 — an UNMATCHED arm (no guard): the exception propagates through the
// finally, which already worked and must keep working exactly once.
BEZEL_TEST(finally_throwing_guard_2881,
           unmatched_arm_propagates_through_finally_spec_31_2) {
    BEZEL_EXPECT_EQ(run(program(
        "  public static method Once(): int "
        "    try "
        "      var s := new Src(); "
        "      var k := s.V(); "
        "      k := k + T.BodyBoom(); "
        "      s.Dispose(); "
        "    catch var e: GX => C._got := C._got + 1000; "
        "    finally "
        "      C._fin := C._fin + 1; "
        "    end; "
        "    return 0; "
        "  end Once; ",
        "    var i := 0; "
        "    while i < 20 do "
        "      try "
        "        var r := C.Once(); "
        "        C._got := C._got + 10000; "
        "      catch GX => C._got := C._got + 100; "
        "      catch BX => C._got := C._got + 1; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 20 then bad := bad + 1; end if; "
        "    if C._got <> 20 then bad := bad + 2; end if; "
        "    if Src._live <> 0 then bad := bad + 4; end if; "
        "    if Src._dtor <> 20 then bad := bad + 8; end if; ")), 0);
}

// §31.2.4 — a non-throwing guard selects the arm, whose body bare-re-raises. The
// arm-body unwind edge (already routed through finUnwindBB) must be undisturbed:
// the finally runs once and the SAME exception (BX) reaches the outer handler.
BEZEL_TEST(finally_throwing_guard_2881,
           arm_body_reraise_after_guard_runs_finally_spec_31_2_4) {
    BEZEL_EXPECT_EQ(run(program(
        "  public static method Once(): int "
        "    var n := 5; "
        "    try "
        "      var s := new Src(); "
        "      var k := s.V(); "
        "      k := k + T.BodyBoom(); "
        "      s.Dispose(); "
        "    catch var e: BX when n > 3 => do "
        "      C._got := C._got + 1000; "
        "      throw; "
        "    end "
        "    finally "
        "      C._fin := C._fin + 1; "
        "    end; "
        "    return 0; "
        "  end Once; ",
        "    var i := 0; "
        "    while i < 20 do "
        "      try "
        "        var r := C.Once(); "
        "        C._got := C._got + 100000; "
        "      catch GX => C._got := C._got + 100; "
        "      catch BX => C._got := C._got + 1; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 20 then bad := bad + 1; end if; "
        "    if C._got <> 20020 then bad := bad + 2; end if; "
        "    if Src._live <> 0 then bad := bad + 4; end if; "
        "    if Src._dtor <> 20 then bad := bad + 8; end if; ")), 0);
}
