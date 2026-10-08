// Regression tests for #2869 — a `try` that has BOTH a `finally` and a catch arm
// that MATCHES skipped every `unwindOnly` disposal registered in the try body.
//
// WHAT THE DOCUMENT REQUIRES.  Spec §28.3 "Local variables, ownership, and
// borrows", subsection *Exception-path discharge* (v0.240):
//
//     Owned locals and owned parameters that are live when an exception
//     propagates out of their enclosing block are disposed implicitly during
//     stack unwinding. The compiler treats every owned local and owned parameter
//     as if it were wrapped in an implicit `try`/`finally` whose `finally`
//     disposes the value if it is still owned (i.e., not already moved-from and
//     not already explicitly disposed).
//
// and, for how that implicit disposal interleaves with an explicit `finally`
// (same subsection, v0.240):
//
//     The implicit auto-dispose composes with explicit `finally` blocks on the
//     single establishment-ordered cleanup stack of §28.10
//
// THE DEFECT (as it stood at v0.240 — the mechanism was retired by #2892, below).
// For a try WITH a `finally`, lowerTry deliberately pushed the try
// body's disposal scope BELOW `unwindScopeDepth_` (#1263), so a throw inside the
// body did NOT dispose that scope at the throw site — the disposal was deferred
// to one of the finally's post-body dispatch blocks, which is what made the
// explicit `finally` run first.  Three of those blocks reached the body scope on
// the UNWIND path (`emitUnwindDisposalsInRange`, isUnwind=true) and were correct.
// The fourth — the edge a catch arm of THIS try leaves by after handling the
// exception — is an ordinary NORMAL exit edge, and every disposal emitted on it
// went through `emitDisposalsInRange` with isUnwind=false.  `emitDisposals`'
// `unwindOnly` arm opens `if (!isUnwind) continue;`, so on that edge every
// `unwindOnly` entry in the try body scope was silently skipped and the value it
// owned leaked.  The gap was therefore precisely: a `try` with BOTH a `finally`
// and a catch arm that matches.
//
// The blast radius is everything registered `unwindOnly` (or the sibling
// `releaseOnUnwindOnly`) in a try body scope: plain owned `var` locals and
// `owned` parameters, the `out owned` recipients, the #993 owned-argument
// temporaries, the #1918 transferred-owned-local transients, the builtin owned
// handles (#1957/#2000), and the #2816 fresh unbound receiver temporary.  It
// covered every exit edge of the arm, not just fall-through: `return`,
// `break`/`continue`, a labeled `break`, and an async method's `return` all
// dispose the body scope through the same `emitDisposalsInRange`.
//
// MEASURED pre-fix on 7c466ee, with a bitmask oracle over twenty-iteration
// loops (bit set == that shape leaked or double-disposed):
//
//   arm fall-through / arm `return` / arm `break` / arm `continue` / nested try
//   / labeled `break` / `when` guard / two body locals / enclosing `using var`
//   / second matching arm / #2816 fresh receiver temp / async `return` from arm
//   / async fall-through from arm      — ALL leaked (320-1616 bytes per program)
//
//   unmatched arm (propagates) / re-raise from the arm / `throw` from the
//   `finally` / genuine normal fall-through / discharge-then-throw /
//   owned-argument transfer-then-throw   — ALL already correct, and must stay so.
//
// THE FIX (as shipped for #2869, since SUPERSEDED — see below).  `isUnwind`
// conflated two questions that diverge on exactly this edge: "did this scope's
// owned locals miss their discharge point?" (what `unwindOnly` /
// `releaseOnUnwindOnly` ask) and "is an exception in flight right now?" (what
// `caughtException`, `asyncDispose`, `taskScope` and #2334's
// `skipCellFreeOnUnwind` ask).  `emitDisposals` gained a separate
// `undischargedOwned` parameter answering only the first; lowerTry marked the try
// body's scope index in `unwoundBodyScopes_` while the catch arms were lowered,
// and a new exit mode 6 disposed the body scope after the finally body.
//
// SUPERSEDED BY #2892 (§28.10 / §31.2, v0.241).  All of that machinery existed
// only because #1263 kept the body scope on the dispose stack past the finally,
// which the owner ruling retired: a `try`'s own `finally` is established at entry
// to the protected block, so the body scope is unwind-disposed AT THE THROW SITE
// and popped before any arm is lowered.  The divergence has no edge left to live
// on, and the #2869 gap is closed STRUCTURALLY rather than by recovery — nothing
// emitted after the throw can name the body scope, so its `unwindOnly` entries
// can neither be skipped (the leak) nor fired twice (the double dispose).  The
// `undischargedOwned` parameter, `unwoundBodyScopes_` and mode 6 were removed
// with the ruling.  Every leak/double-dispose oracle below is unchanged and
// still fails if either direction regresses; only the ORDERING pin moved (213 ->
// 321, at the end of this file).
//
// THE ORACLE IS A LIVE-OBJECT COUNTER, NOT "NO LEAK".  The direction this fix can
// break is a double dispose, which LeakSanitizer cannot see.  Every program below
// folds a live count AND a destructor count into the exit code (`_live` must be
// 0 and `_dtor` must equal the number of objects created), and runs under
// generated-code ASan+LSan — so an over-release faults or shows a wrong `_dtor`,
// and an under-release leaks.  Every loop is amplified to 20 iterations: a
// one-shot `Main` is not a reliable LSan oracle in this tree.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// ASan + LSan on the generated program. A leak, double-free or UAF surfaces as
// ran=false (-999); otherwise the exit code.
int run(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// The ordering oracle's VALUE CHANNEL. Its observable is a sequence value, and
// since the #2892 re-derivation that value (321) exceeds the 8-bit exit-code
// ceiling — a process exit status is masked to WEXITSTATUS, so it must travel on
// stdout (tests/README.md). Still ASan/LSan-instrumented, so a leak or double
// dispose shows up as "<did-not-run>" rather than as a shifted digit.
std::string runOrderingOut(const std::string& program) {
    auto r = runAotChecked(program);
    if (!r.ran) return std::string("<did-not-run>");
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
        if (!out.empty() && out.back() == '\r') out.pop_back();
    }
    return out;
}

// `Src` counts live instances AND destructor runs, so the oracle distinguishes a
// leak (live > 0) from a double dispose (dtor > created). `Boom` is a throwing
// property getter — the #2816 fresh-receiver-temporary shape.
const char* kPrelude =
    "module M; "
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
    "  public property Boom: int "
    "    get => do throw new InvalidOperationException(\"no\"); end; "
    "  end Boom; "
    "end Src; "
    "class T "
    "  public static method Boom(): int "
    "    throw new InvalidOperationException(\"no\"); "
    "  end Boom; "
    "  public static method Take(owned s: Src): int "
    "    var r := s.V(); "
    "    s.Dispose(); "
    "    return r; "
    "  end Take; "
    "end T; ";

// A program whose `Main` runs `body` (which must leave `bad` at 0 and is
// responsible for its own loop), then checks the counters.
// `made` is how many Src instances the body is expected to have created.
std::string program(const std::string& members, const std::string& mainBody) {
    return std::string(kPrelude) +
           "class C "
           "  public static _fin: int := 0; " +
           members +
           "  public static method Main(): int " +
           "    var bad := 0; "
           "    Src._live := 0; Src._dtor := 0; C._fin := 0; " +
           mainBody +
           "    return bad; "
           "  end Main; "
           "end C;";
}

// The canonical shape: 20 iterations of a try whose body creates one owned Src,
// discharges it on the normal path (so codegen registers `unwindOnly`, not the
// auto-drop `dynamicDispose`), and throws before that discharge; `arm` is the
// catch-arm body and `tail` the per-iteration statement after the try.
std::string loopBody(const std::string& arm, const std::string& tail,
                     const std::string& finallyBody, int wantFin, int wantMade) {
    return
        "    var i := 0; "
        "    while i < 20 do "
        "      try "
        "        var s := new Src(); "
        "        var k := s.V(); "
        "        k := k + T.Boom(); "
        "        s.Dispose(); "
        "      catch var e: InvalidOperationException => " + arm +
        "      finally " + finallyBody +
        "      end; " + tail +
        "    end while; "
        "    if C._fin <> " + std::to_string(wantFin) + " then bad := bad + 1; end if; "
        "    if Src._live <> 0 then bad := bad + 2; end if; "
        "    if Src._dtor <> " + std::to_string(wantMade) + " then bad := bad + 4; end if; ";
}

const char* kFinCount = "C._fin := C._fin + 1; ";

}  // namespace

// ===========================================================================
// FAIL-WITHOUT / PASS-WITH — every exit edge of a MATCHING catch arm of a
// try-with-finally.  Each returned a non-zero bitmask pre-fix (bit 2 = the
// object was still live, bit 4 = its destructor never ran); 0 post-fix.
// ===========================================================================

// §28.3 Exception-path discharge — the issue's own repro: the arm handles the
// exception and FALLS THROUGH to the finally.
BEZEL_TEST(finally_caught_arm_unwind_2869,
           arm_fallthrough_disposes_body_owned_local_spec_28_3) {
    BEZEL_EXPECT_EQ(run(program(
        "", loopBody("i := i + 0; ", "i := i + 1; ", kFinCount, 20, 20))), 0);
}

// §28.3 / §31.2 — the arm RETURNS (exit mode 1): the body scope is disposed at
// the return SITE, which is the same emitDisposalsInRange the fall-through uses.
BEZEL_TEST(finally_caught_arm_unwind_2869,
           arm_return_disposes_body_owned_local_spec_28_3) {
    BEZEL_EXPECT_EQ(run(program(
        "  public static method Once(): int "
        "    try "
        "      var s := new Src(); "
        "      var k := s.V(); "
        "      k := k + T.Boom(); "
        "      s.Dispose(); "
        "      return 1; "
        "    catch var e: InvalidOperationException => "
        "      return 0; "
        "    finally "
        "      C._fin := C._fin + 1; "
        "    end; "
        "  end Once; ",
        "    var i := 0; "
        "    while i < 20 do "
        "      if C.Once() <> 0 then bad := bad + 8; end if; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 20 then bad := bad + 1; end if; "
        "    if Src._live <> 0 then bad := bad + 2; end if; "
        "    if Src._dtor <> 20 then bad := bad + 4; end if; ")), 0);
}

// §28.3 / §29.7 — the arm BREAKS out of the enclosing loop (exit mode 2).
BEZEL_TEST(finally_caught_arm_unwind_2869,
           arm_break_disposes_body_owned_local_spec_28_3) {
    BEZEL_EXPECT_EQ(run(program(
        "", loopBody("do i := 100; break; end ", "i := i + 1; ",
                     kFinCount, 1, 1) +
            "    if i <> 100 then bad := bad + 8; end if; ")), 0);
}

// §28.3 / §29.7 — the arm CONTINUEs the enclosing loop (exit mode 3).
BEZEL_TEST(finally_caught_arm_unwind_2869,
           arm_continue_disposes_body_owned_local_spec_28_3) {
    BEZEL_EXPECT_EQ(run(program(
        "", loopBody("do i := i + 1; continue; end ", "i := i + 1; ",
                     kFinCount, 20, 20))), 0);
}

// §21.5 (v0.201) — a LABELED `break` out of an OUTER loop from the arm. This is
// the exit-mode family whose numbering floor the fix moved (6 -> 7), so it also
// pins that the labeled routes still dispatch correctly.
BEZEL_TEST(finally_caught_arm_unwind_2869,
           arm_labeled_break_disposes_body_owned_local_spec_21_5) {
    BEZEL_EXPECT_EQ(run(program(
        "",
        "    var i := 0; "
        "    scan: while i < 20 do "
        "      var j := 0; "
        "      while j < 3 do "
        "        try "
        "          var s := new Src(); "
        "          var k := s.V(); "
        "          k := k + T.Boom(); "
        "          s.Dispose(); "
        "        catch var e: InvalidOperationException => do "
        "          i := 100; "
        "          break scan; "
        "        end "
        "        finally "
        "          C._fin := C._fin + 1; "
        "        end; "
        "        j := j + 1; "
        "      end while; "
        "      i := i + 1; "
        "    end scan; "
        "    if C._fin <> 1 then bad := bad + 1; end if; "
        "    if Src._live <> 0 then bad := bad + 2; end if; "
        "    if Src._dtor <> 1 then bad := bad + 4; end if; "
        "    if i <> 100 then bad := bad + 8; end if; ")), 0);
}

// §28.3 — a NESTED try: the inner try (catch + finally) sits inside an outer
// try's body, so the inner body scope is marked while an outer body scope is
// live. Both owned locals must be disposed exactly once.
BEZEL_TEST(finally_caught_arm_unwind_2869,
           nested_try_inner_arm_disposes_both_spec_28_3) {
    BEZEL_EXPECT_EQ(run(program(
        "",
        "    var i := 0; "
        "    while i < 20 do "
        "      try "
        "        var outerv := new Src(); "
        "        try "
        "          var s := new Src(); "
        "          var k := s.V(); "
        "          k := k + T.Boom(); "
        "          s.Dispose(); "
        "        catch var e: InvalidOperationException => "
        "          i := i + 0; "
        "        finally "
        "          C._fin := C._fin + 1; "
        "        end; "
        "        outerv.Dispose(); "
        "      finally "
        "        C._fin := C._fin + 1; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 40 then bad := bad + 1; end if; "
        "    if Src._live <> 0 then bad := bad + 2; end if; "
        "    if Src._dtor <> 40 then bad := bad + 4; end if; ")), 0);
}

// §28.3 *Unbound owned temporaries* — the #2816 fresh unbound RECEIVER temporary
// rides the same `unwindOnly` machinery, registered into the try body scope at
// its production. This is the shape the #2816 lane hit that led to this issue.
BEZEL_TEST(finally_caught_arm_unwind_2869,
           fresh_receiver_temp_disposed_on_caught_arm_spec_28_3) {
    BEZEL_EXPECT_EQ(run(program(
        "",
        "    var i := 0; "
        "    while i < 20 do "
        "      try "
        "        var c := (new Src()).Boom; "
        "        i := i + c; "
        "      catch var e: InvalidOperationException => "
        "        i := i + 0; "
        "      finally "
        "        C._fin := C._fin + 1; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 20 then bad := bad + 1; end if; "
        "    if Src._live <> 0 then bad := bad + 2; end if; "
        "    if Src._dtor <> 20 then bad := bad + 4; end if; ")), 0);
}

// §28.3 / §31.2 — a `when` GUARD on the matching arm (the arm is entered through
// the prep/guard chain, not the plain type test).
BEZEL_TEST(finally_caught_arm_unwind_2869,
           arm_with_when_guard_disposes_body_owned_local_spec_31_2) {
    BEZEL_EXPECT_EQ(run(program(
        "",
        "    var i := 0; "
        "    while i < 20 do "
        "      try "
        "        var s := new Src(); "
        "        var k := s.V(); "
        "        k := k + T.Boom(); "
        "        s.Dispose(); "
        "      catch var e: InvalidOperationException when i >= 0 => "
        "        i := i + 0; "
        "      finally "
        "        C._fin := C._fin + 1; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 20 then bad := bad + 1; end if; "
        "    if Src._live <> 0 then bad := bad + 2; end if; "
        "    if Src._dtor <> 20 then bad := bad + 4; end if; ")), 0);
}

// §30.9 / §28.3 — an ASYNC method: `return` from the arm is exit mode 5 and the
// fall-through is the new mode 6, both inside a state machine.
BEZEL_TEST(finally_caught_arm_unwind_2869,
           async_arm_exits_dispose_body_owned_local_spec_30_9) {
    BEZEL_EXPECT_EQ(run(std::string(kPrelude) +
        "class C "
        "  public static _fin: int := 0; "
        "  public static async method Ret(): Task<int> "
        "    try "
        "      var s := new Src(); "
        "      var k := s.V(); "
        "      k := k + T.Boom(); "
        "      s.Dispose(); "
        "      return 1; "
        "    catch var e: InvalidOperationException => "
        "      return 0; "
        "    finally "
        "      C._fin := C._fin + 1; "
        "    end; "
        "  end Ret; "
        "  public static async method Fall(): Task<int> "
        "    var r := 7; "
        "    try "
        "      var s := new Src(); "
        "      var k := s.V(); "
        "      k := k + T.Boom(); "
        "      s.Dispose(); "
        "    catch var e: InvalidOperationException => "
        "      r := 0; "
        "    finally "
        "      C._fin := C._fin + 1; "
        "    end; "
        "    return r; "
        "  end Fall; "
        "  public static async method Main(): Task<int> "
        "    var bad := 0; var i := 0; "
        "    Src._live := 0; Src._dtor := 0; C._fin := 0; "
        "    while i < 20 do "
        "      var a := await C.Ret(); "
        "      if a <> 0 then bad := bad + 8; end if; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 20 then bad := bad + 1; end if; "
        "    if Src._live <> 0 then bad := bad + 2; end if; "
        "    if Src._dtor <> 20 then bad := bad + 4; end if; "
        "    Src._live := 0; Src._dtor := 0; C._fin := 0; i := 0; "
        "    while i < 20 do "
        "      var b := await C.Fall(); "
        "      if b <> 0 then bad := bad + 128; end if; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 20 then bad := bad + 16; end if; "
        "    if Src._live <> 0 then bad := bad + 32; end if; "
        "    if Src._dtor <> 20 then bad := bad + 64; end if; "
        "    return bad; "
        "  end Main; "
        "end C;"), 0);
}

// §28.13 / §30.15 (#1957/#2000) — a builtin owned HANDLE (`Shared<T>`) declared
// in the try body and transferred to an `owned` parameter whose LATER argument
// throws. Its transient rides `releaseOnUnwindOnly`, the sibling flag of
// `unwindOnly`, and was skipped on the same edge: the handle was never released,
// so the shared payload's destructor never ran.
BEZEL_TEST(finally_caught_arm_unwind_2869,
           builtin_shared_handle_released_on_caught_arm_spec_28_13) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class X "
        "  public static _n: int := 0; "
        "  public constructor() end constructor; "
        "  destructor X._n := X._n + 1; end destructor; "
        "end X; "
        "class P "
        "  public static _fin: int := 0; "
        "  public static method Sink(owned h: Shared<X>, n: int): int "
        "    h.Dispose(); return n; "
        "  end Sink; "
        "  public static method Boom(): int throw new Exception(\"boom\"); end Boom; "
        "  public static method Main(): int "
        "    var bad := 0; var i := 0; "
        "    X._n := 0; P._fin := 0; "
        "    while i < 20 do "
        "      try "
        "        var s := new Shared<X>(new X()); "
        "        P.Sink(move s, P.Boom()); "
        "      catch Exception => i := i + 0; "
        "      finally "
        "        P._fin := P._fin + 1; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    if P._fin <> 20 then bad := bad + 1; end if; "
        "    if X._n <> 20 then bad := bad + 2; end if; "
        "    return bad; "
        "  end Main; "
        "end P;"), 0);
}

// §22.3 / §22.4 — an `out owned var` recipient declared in the try body (its
// disposal is registered `unwindOnly` at the call, LlvmBackend_Calls2.cpp), with
// the throw after the call and before the recipient's discharge.
BEZEL_TEST(finally_caught_arm_unwind_2869,
           out_owned_recipient_disposed_on_caught_arm_spec_22_3) {
    BEZEL_EXPECT_EQ(run(program(
        "  public static method Make(out owned o: Src): int "
        "    o := new Src(); "
        "    return 1; "
        "  end Make; ",
        "    var i := 0; "
        "    while i < 20 do "
        "      try "
        "        var r := C.Make(out owned var q); "
        "        r := r + T.Boom(); "
        "        q.Dispose(); "
        "      catch var e: InvalidOperationException => "
        "        i := i + 0; "
        "      finally "
        "        C._fin := C._fin + 1; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 20 then bad := bad + 1; end if; "
        "    if Src._live <> 0 then bad := bad + 2; end if; "
        "    if Src._dtor <> 20 then bad := bad + 4; end if; ")), 0);
}

// ===========================================================================
// ORDERING — WHERE the recovered disposal runs.
//
// RE-DERIVED for #2892 (§28.10 / §31.2, v0.241; text current at v0.252): "A
// `try` statement's own `finally` is established at entry to the protected
// block ... an owned local or `using var` declared inside the protected block
// (established later) is disposed before the `finally` runs" on every exit edge.
// The body local is therefore disposed as the exception leaves the protected
// block — before the arm that handles it and before the finally: dtor=3,
// catch=2, finally=1 -> 321.  It is still the same single disposal this file
// exists to prove happens (pre-#2869 the destructor never ran at all, giving
// 21); only its POSITION moved, from after the finally to before the arm.
//
// The value pinned between #2869 and #2892 was 213 (catch, finally, dtor),
// matching #1263's retired `catch_then_finally_then_autodispose` ordering; that
// ordering was ruled against in v0.241 and the whole family re-derived in
// tests/codegen/finally_established_at_try_entry_2892_test.cpp.
// ===========================================================================

BEZEL_TEST(finally_caught_arm_unwind_2869,
           recovered_disposal_runs_before_the_finally_spec_28_10) {
    BEZEL_EXPECT_EQ(runOrderingOut(
        "module M; "
        "class R "
        "  private _v: int := 5; "
        "  destructor C._seq := C._seq * 10 + 3; end destructor; "
        "  public method V(): int return _v; end V; "
        "end R; "
        "class T "
        "  public static method Boom(): int "
        "    throw new InvalidOperationException(\"no\"); "
        "  end Boom; "
        "end T; "
        "class C "
        "  public static _seq: int := 0; "
        "  public static method Main(): int "
        "    C._seq := 0; "
        "    try "
        "      var r := new R(); "
        "      var k := r.V(); "
        "      k := k + T.Boom(); "
        "      r.Dispose(); "
        "    catch var e: InvalidOperationException => "
        "      C._seq := C._seq * 10 + 2; "
        "    finally "
        "      C._seq := C._seq * 10 + 1; "
        "    end; "
        "    Console.WriteLine(C._seq); "
        "    return 0; "
        "  end Main; "
        "end C;"), "321");
}

// ===========================================================================
// ACCEPT CONTROLS — the direction this fix can BREAK.  Firing an `unwindOnly`
// entry on a genuine normal edge is a DOUBLE dispose, which LSan cannot see; the
// `_dtor` count in each program is what catches it.  All of these were already
// correct pre-fix and must stay correct.
// ===========================================================================

// §28.3 — the genuine normal fall-through of a try that HAS both a catch and a
// finally: nothing throws, the body local is discharged by the program, and the
// mode-0 dispatch must keep skipping the `unwindOnly` entry.
BEZEL_TEST(finally_caught_arm_unwind_2869,
           normal_fallthrough_disposes_exactly_once_spec_28_3) {
    BEZEL_EXPECT_EQ(run(program(
        "",
        "    var i := 0; "
        "    while i < 20 do "
        "      try "
        "        var s := new Src(); "
        "        var k := s.V(); "
        "        s.Dispose(); "
        "        i := i + k - 4; "
        "      catch var e: InvalidOperationException => "
        "        i := i + 100; "
        "      finally "
        "        C._fin := C._fin + 1; "
        "      end; "
        "    end while; "
        "    if C._fin <> 20 then bad := bad + 1; end if; "
        "    if Src._live <> 0 then bad := bad + 2; end if; "
        "    if Src._dtor <> 20 then bad := bad + 4; end if; ")), 0);
}

// §28.3 — DISCHARGE (explicit `Dispose()`) and THEN throw, caught by this try's
// own arm. The recovered pass must find the slot already nulled by the discharge:
// a second dispose here would be a double free.
BEZEL_TEST(finally_caught_arm_unwind_2869,
           discharge_then_throw_disposes_exactly_once_spec_28_3) {
    BEZEL_EXPECT_EQ(run(program(
        "",
        "    var i := 0; "
        "    while i < 20 do "
        "      try "
        "        var s := new Src(); "
        "        var k := s.V(); "
        "        s.Dispose(); "
        "        k := k + T.Boom(); "
        "      catch var e: InvalidOperationException => "
        "        i := i + 0; "
        "      finally "
        "        C._fin := C._fin + 1; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 20 then bad := bad + 1; end if; "
        "    if Src._live <> 0 then bad := bad + 2; end if; "
        "    if Src._dtor <> 20 then bad := bad + 4; end if; ")), 0);
}

// §28.3 / §22.3 — the #1918 shape: the body local is TRANSFERRED to an `owned`
// parameter (which disposes it) and the throw follows on the same path.
BEZEL_TEST(finally_caught_arm_unwind_2869,
           owned_arg_transfer_then_throw_disposes_exactly_once_spec_28_3) {
    BEZEL_EXPECT_EQ(run(program(
        "",
        "    var i := 0; "
        "    while i < 20 do "
        "      try "
        "        var s := new Src(); "
        "        var k := T.Take(move s); "
        "        k := k + T.Boom(); "
        "      catch var e: InvalidOperationException => "
        "        i := i + 0; "
        "      finally "
        "        C._fin := C._fin + 1; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 20 then bad := bad + 1; end if; "
        "    if Src._live <> 0 then bad := bad + 2; end if; "
        "    if Src._dtor <> 20 then bad := bad + 4; end if; ")), 0);
}

// §28.3 — BRANCHED discharge: on odd iterations the local is discharged and a
// throw follows; on even iterations it is discharged and the body falls through.
// Both runtime edges must dispose exactly once through the same emitted blocks.
BEZEL_TEST(finally_caught_arm_unwind_2869,
           branched_discharge_disposes_exactly_once_spec_28_3) {
    BEZEL_EXPECT_EQ(run(program(
        "",
        "    var i := 0; "
        "    while i < 20 do "
        "      try "
        "        var s := new Src(); "
        "        var k := s.V(); "
        "        s.Dispose(); "
        "        if i mod 2 = 1 then "
        "          k := k + T.Boom(); "
        "        end if; "
        "      catch var e: InvalidOperationException => "
        "        i := i + 0; "
        "      finally "
        "        C._fin := C._fin + 1; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 20 then bad := bad + 1; end if; "
        "    if Src._live <> 0 then bad := bad + 2; end if; "
        "    if Src._dtor <> 20 then bad := bad + 4; end if; ")), 0);
}

// §28.3 / §31.2 — NO arm matches, so the exception propagates through the
// finally on the genuine unwind edge (`finUnwindBB`, isUnwind=true). Already
// correct pre-fix; the mode-6 addition must not disturb it.
BEZEL_TEST(finally_caught_arm_unwind_2869,
           unmatched_arm_propagates_disposes_exactly_once_spec_28_3) {
    BEZEL_EXPECT_EQ(run(program(
        "  public static method Once(): int "
        "    try "
        "      var s := new Src(); "
        "      var k := s.V(); "
        "      k := k + T.Boom(); "
        "      s.Dispose(); "
        "      return 1; "
        "    catch var e: ArgumentException => "
        "      return 2; "
        "    finally "
        "      C._fin := C._fin + 1; "
        "    end; "
        "  end Once; ",
        "    var i := 0; "
        "    while i < 20 do "
        "      try "
        "        var r := C.Once(); "
        "        if r <> 0 then bad := bad + 8; end if; "
        "      catch var e2: InvalidOperationException => "
        "        i := i + 0; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 20 then bad := bad + 1; end if; "
        "    if Src._live <> 0 then bad := bad + 2; end if; "
        "    if Src._dtor <> 20 then bad := bad + 4; end if; ")), 0);
}

// §31.2 — the arm RE-RAISES a new exception, so control leaves through
// `finUnwindBB` rather than through the mode dispatch. Already correct pre-fix.
BEZEL_TEST(finally_caught_arm_unwind_2869,
           rethrow_from_arm_disposes_exactly_once_spec_31_2) {
    BEZEL_EXPECT_EQ(run(program(
        "  public static method Once(): int "
        "    try "
        "      var s := new Src(); "
        "      var k := s.V(); "
        "      k := k + T.Boom(); "
        "      s.Dispose(); "
        "      return 1; "
        "    catch var e: InvalidOperationException => "
        "      throw new ArgumentException(\"re\"); "
        "    finally "
        "      C._fin := C._fin + 1; "
        "    end; "
        "  end Once; ",
        "    var i := 0; "
        "    while i < 20 do "
        "      try "
        "        var r := C.Once(); "
        "        if r <> 0 then bad := bad + 8; end if; "
        "      catch var e2: ArgumentException => "
        "        i := i + 0; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 20 then bad := bad + 1; end if; "
        "    if Src._live <> 0 then bad := bad + 2; end if; "
        "    if Src._dtor <> 20 then bad := bad + 4; end if; ")), 0);
}

// §31.2 — the FINALLY throws, superseding the handled exception, after the arm
// already ran. The body scope is then disposed by the finally-throw's own unwind
// pass; the mode-6 block is unreachable on that path (no double dispose).
BEZEL_TEST(finally_caught_arm_unwind_2869,
           throw_from_finally_after_arm_disposes_exactly_once_spec_31_2) {
    BEZEL_EXPECT_EQ(run(program(
        "  public static method Once(): int "
        "    try "
        "      var s := new Src(); "
        "      var k := s.V(); "
        "      k := k + T.Boom(); "
        "      s.Dispose(); "
        "      return 1; "
        "    catch var e: InvalidOperationException => "
        "      return 2; "
        "    finally "
        "      C._fin := C._fin + 1; "
        "      throw new ArgumentException(\"fin\"); "
        "    end; "
        "  end Once; ",
        "    var i := 0; "
        "    while i < 20 do "
        "      try "
        "        var r := C.Once(); "
        "        if r <> 0 then bad := bad + 8; end if; "
        "      catch var e2: ArgumentException => "
        "        i := i + 0; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 20 then bad := bad + 1; end if; "
        "    if Src._live <> 0 then bad := bad + 2; end if; "
        "    if Src._dtor <> 20 then bad := bad + 4; end if; ")), 0);
}

// §28.10 — a `using var` (a `dynamicDispose` entry, which fires on BOTH edges)
// declared alongside the discharged plain owned local in the same try body. The
// `using var` must still be disposed exactly once now that the scope's pass also
// discharges the `unwindOnly` entry.
BEZEL_TEST(finally_caught_arm_unwind_2869,
           using_var_beside_owned_local_disposes_exactly_once_spec_28_10) {
    BEZEL_EXPECT_EQ(run(program(
        "",
        "    var i := 0; "
        "    while i < 20 do "
        "      try "
        "        using var a := new Src(); "
        "        var s := new Src(); "
        "        var k := s.V() + a.V(); "
        "        k := k + T.Boom(); "
        "        s.Dispose(); "
        "      catch var e: InvalidOperationException => "
        "        i := i + 0; "
        "      finally "
        "        C._fin := C._fin + 1; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 20 then bad := bad + 1; end if; "
        "    if Src._live <> 0 then bad := bad + 2; end if; "
        "    if Src._dtor <> 40 then bad := bad + 4; end if; ")), 0);
}

// §28.10 — an ENCLOSING `using var` established BEFORE the try (a scope OUTSIDE
// the marked body scope) must be untouched by the mark: it is disposed once, at
// the end of its own block, not by the try's dispatch.
BEZEL_TEST(finally_caught_arm_unwind_2869,
           enclosing_using_var_disposes_exactly_once_spec_28_10) {
    BEZEL_EXPECT_EQ(run(program(
        "",
        "    var i := 0; "
        "    while i < 20 do "
        "      do "
        "        using var outerv := new Src(); "
        "        try "
        "          var s := new Src(); "
        "          var k := s.V() + outerv.V(); "
        "          k := k + T.Boom(); "
        "          s.Dispose(); "
        "        catch var e: InvalidOperationException => "
        "          i := i + 0; "
        "        finally "
        "          C._fin := C._fin + 1; "
        "        end; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    if C._fin <> 20 then bad := bad + 1; end if; "
        "    if Src._live <> 0 then bad := bad + 2; end if; "
        "    if Src._dtor <> 40 then bad := bad + 4; end if; ")), 0);
}
