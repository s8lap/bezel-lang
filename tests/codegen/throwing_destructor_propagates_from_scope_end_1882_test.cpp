// Codegen — issue #1882 (and #1878's remaining disposal points) / spec §28.4
// "The `destructor`", docs revision v0.306.
//
// §28.4 (v0.306), the throwing-destructor paragraph:
//   "- The exception propagates from the **disposal point** — the explicit
//      `Dispose()` call, the `using var`/auto-drop scope-end site, the
//      drop-on-overwrite site (§28.7), the owning container's cascade, or an
//      unbound owned temporary's end-of-full-expression (or unwind) disposal
//      (§28.3).
//    - ...
//    - In a **multi-value cleanup pass** ... a throwing disposer does not skip
//      the remaining values: every remaining disposal still runs. If a later
//      disposal in the same pass also throws, the later exception
//      **supersedes** the earlier one ..."
//
// VERIFIED ON HEAD 7f29b8ce (both of the issue's programs, compiled + run):
//   * the repro (`using var` disposed inside a `try`/`catch Exception`):
//     `Unhandled exception: Exception: dtor`, exit 70 — the exception ESCAPED
//     the enclosing catch;
//   * the issue's CONTROL 2 (the same disposal at method top level, no
//     handler): fell through to `return 55`, exit 55 — SILENTLY SWALLOWED.
// Two opposite outcomes from one disposal site, flipping on semantically
// irrelevant surrounding code. Both are the same omission: the scope-end
// disposal pass never looked at the bz_exc slot the generated `DisposeCore`
// parks the fault in (#1546's `dtor.threw`/`dtor.restore` tail). With no later
// pending check in the function the fault was swallowed; with one — any later
// call, typically the §22.13 static-init guard of the next static access — it
// surfaced at that unrelated site, which lies OUTSIDE the `try` that lexically
// contained the disposal point, so it escaped as unhandled.
//
// ROOT CAUSE / FIX: `Emitter::emitDisposals`
// (src/codegen/LlvmBackend_Disposal.cpp) now ends a NON-unwind pass that could
// run user code with `emitPendingCheck`, so the fault propagates from the
// disposal point. It is emitted once at the END of the pass, never per entry,
// because §28.4 requires the remaining values to be disposed anyway; the
// per-object supersession between them is `DisposeCore`'s own entry-exception
// tail (#2358). The unwind edge is given the pass's own scope index so it
// disposes only the scopes OUTSIDE the one being torn down —
// `emitPendingCheckIf`/`emitPendingCheck` gained that parameter, defaulting to
// the all-live-scopes meaning every pre-existing caller has. A pass over a
// FREE-STANDING vector — not on `disposeStack_` at all — is not a scope end and
// is excluded entirely; that route is the §29.7 abandonment teardown, pinned by
// the two `..._does_not_leak_the_frame_spec_29_7` rows below.
//
// The same pass closes #1878's two remaining disposal points — its "Second
// disposal point" (`using var` scope-end) and its "OwningArray element
// cascade" sentence; #1878's own explicit-`Dispose()` repro is pinned in
// throwing_destructor_propagates_from_explicit_dispose_1878_test.cpp.
//
// FAILS WITHOUT / PASSES WITH: every row except the two controls flips —
// confirmed by reverting the LlvmBackend_Disposal.cpp / LlvmBackend_impl.hpp
// hunks, rebuilding, and re-running this filter.

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// §33.3: the uncaught-exception boundary prints type + message to stderr and
// exits 70 (EX_SOFTWARE).
constexpr int kUnhandledExceptionExit = 70;

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// §28.4 — THE ISSUE'S VERBATIM REPRO. `using var b`'s scope-end disposal sits
// lexically inside the `try`, so `catch Exception` must receive the fault. The
// observable is the weighted return: 100 + 10*C.caught + BadDtor.ran == 111
// proves the arm ran (caught==1) AND the destructor body ran exactly once
// (ran==1). Pre-fix: exit 70, the exception escaping the catch as unhandled.
BEZEL_TEST(throwing_destructor_propagates_from_scope_end_1882,
           using_var_scope_end_propagates_to_enclosing_catch_spec_28_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class BadDtor "
        "  public static ran: int := 0; "
        "  public constructor() end constructor; "
        "  destructor "
        "    BadDtor.ran := BadDtor.ran + 1; "
        "    throw new Exception(\"dtor\"); "
        "  end destructor; "
        "end BadDtor; "
        "class C "
        "  public static caught: int := 0; "
        "  public static method Main(): int "
        "    BadDtor.ran := 0; "
        "    try "
        "      do "
        "        using var b := new BadDtor(); "
        "        var _z := 0; "
        "      end; "
        "    catch Exception => C.caught := 1; "
        "    end; "
        "    return 100 + C.caught * 10 + BadDtor.ran; "
        "  end Main; "
        "end C;"), 111);
}

// §28.4 + §33.3 — THE ISSUE'S CONTROL 2, the opposite manifestation: the same
// scope-end disposal with NO handler must propagate out of `Main` and reach the
// uncaught-exception boundary. Pre-fix it was silently swallowed and execution
// fell through to `return 55`. The stdout assertion carries the weight: an
// "exit != 0" oracle would have passed on the pre-fix `return 55` too, and the
// "fellthrough" line is exactly the statement that must NOT run.
BEZEL_TEST(throwing_destructor_propagates_from_scope_end_1882,
           unhandled_scope_end_fault_is_not_swallowed_spec_33_3) {
    auto r = runAot(
        "module M; "
        "class BadDtor "
        "  public constructor() end constructor; "
        "  destructor "
        "    Console.WriteLine(\"dtor\"); "
        "    throw new Exception(\"dtor\"); "
        "  end destructor; "
        "end BadDtor; "
        "class C "
        "  public static method Main(): int "
        "    do "
        "      using var b := new BadDtor(); "
        "      var _z := 0; "
        "    end; "
        "    Console.WriteLine(\"fellthrough\"); "
        "    return 55; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kUnhandledExceptionExit);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("dtor\n"));
}

// §28.4 — the fault propagates FROM THE DISPOSAL POINT, so the statement that
// follows the `do ... end` block inside the same `try` must not run. Full
// stdout transcript: "in-scope" (the block body), "dtor 2" (the disposal),
// "caught" (the arm), "after" (resumption past the `try`) — and no "no-throw".
// This is #1878's "Second disposal point" program, closed by the same pass.
BEZEL_TEST(throwing_destructor_propagates_from_scope_end_1882,
           statement_after_the_disposal_point_does_not_run_spec_28_4) {
    auto r = runAot(
        "module M; "
        "class Res "
        "  public id: int; "
        "  public constructor(x: int) id := x; end constructor; "
        "  destructor "
        "    Console.WriteLine(\"dtor \" + id.ToString()); "
        "    throw new Exception(\"cleanup-failed\"); "
        "  end destructor; "
        "end Res; "
        "class Program "
        "  public static method Main(): int "
        "    try "
        "      do "
        "        using var r := new Res(2); "
        "        Console.WriteLine(\"in-scope\"); "
        "      end; "
        "      Console.WriteLine(\"no-throw\"); "
        "    catch Exception => Console.WriteLine(\"caught\") "
        "    end; "
        "    Console.WriteLine(\"after\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("in-scope\ndtor 2\ncaught\nafter\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.4 MULTI-VALUE CLEANUP PASS, on the NORMAL exit edge (the #2358 rows cover
// the unwind edge). Two `using var`s with throwing destructors: reverse
// declaration order disposes `a` first (AFault), and §28.4 forbids skipping the
// rest, so `b` is disposed too (BFault) and BFault SUPERSEDES AFault. The
// transcript pins that both bodies ran, in that order; the exit code pins which
// exception survived to the handler. Checking per-entry instead of once at the
// end of the pass would have produced "A" only and exit 11.
BEZEL_TEST(throwing_destructor_propagates_from_scope_end_1882,
           multi_value_pass_runs_every_disposal_and_later_supersedes_spec_28_4) {
    auto r = runAot(
        "module M; "
        "class AFault: Exception "
        "  public constructor() base(\"a-fault\"); end constructor; "
        "end AFault; "
        "class BFault: Exception "
        "  public constructor() base(\"b-fault\"); end constructor; "
        "end BFault; "
        "class AThrows "
        "  destructor Console.WriteLine(\"A\"); throw new AFault(); end destructor; "
        "end AThrows; "
        "class BThrows "
        "  destructor Console.WriteLine(\"B\"); throw new BFault(); end destructor; "
        "end BThrows; "
        "class C "
        "  public static method Main(): int "
        "    try "
        "      do "
        "        using var b := new BThrows(); "
        "        using var a := new AThrows(); "
        "        Console.WriteLine(\"body\"); "
        "      end; "
        "      return 1; "
        "    catch "
        "      var ex: AFault => do return 11; end, "
        "      var ex: BFault => do return 12; end "
        "    end; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("body\nA\nB\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 12);   // BFault (the later disposal) survived
}

// §28.4 — "the owning container's cascade" is a disposal point in the same
// bullet (#1878's third sentence). `OwningArray<Elem>`'s scope-end cascade runs
// each element's destructor through the runtime free loop; a throwing element
// must not be swallowed. The transcript pins that all three elements were
// disposed despite the first fault ("a throwing disposer does not skip the
// remaining values"), and the exit code pins that the surviving fault reached
// the arm. Pre-fix: "elem 3/2/1" printed and then exit 1 — fully swallowed.
BEZEL_TEST(throwing_destructor_propagates_from_scope_end_1882,
           owning_container_element_cascade_propagates_spec_28_4) {
    auto r = runAot(
        "module M; "
        "class EFault: Exception "
        "  public constructor() base(\"e-fault\"); end constructor; "
        "end EFault; "
        "class Elem "
        "  public id: int; "
        "  public constructor(x: int) id := x; end constructor; "
        "  destructor "
        "    Console.WriteLine(\"elem \" + id.ToString()); "
        "    throw new EFault(); "
        "  end destructor; "
        "end Elem; "
        "class C "
        "  public static method Main(): int "
        "    try "
        "      do "
        "        using var arr := new OwningArray<Elem>(); "
        "        arr.Add(new Elem(1)); "
        "        arr.Add(new Elem(2)); "
        "        arr.Add(new Elem(3)); "
        "        Console.WriteLine(\"filled\"); "
        "      end; "
        "      return 1; "
        "    catch "
        "      var ex: EFault => do return 11; end "
        "    end; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("filled\nelem 3\nelem 2\nelem 1\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 11);
}

// §28.4 — "physical reclamation still occurs — a cleanup fault never leaks the
// object", under ASan/LSan and LOOP-AMPLIFIED: a one-shot leak row can pass
// vacuously because the single allocation stays reachable from a live stack
// slot. 200 iterations of (dispose → fault → handle) must leave nothing behind:
// not the disposed object, not the exception the handler consumed. The exit
// code independently pins that the arm ran on every iteration.
BEZEL_TEST(throwing_destructor_propagates_from_scope_end_1882,
           repeated_scope_end_faults_leak_nothing_spec_28_4) {
    auto r = runAotChecked(
        "module M; "
        "class Fault: Exception "
        "  public constructor() base(\"fault\"); end constructor; "
        "end Fault; "
        "class R "
        "  destructor throw new Fault(); end destructor; "
        "end R; "
        "class C "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var i in 0..<200 do "
        "      try "
        "        do "
        "          using var r := new R(); "
        "        end; "
        "      catch Fault => n := n + 1; "
        "      end; "
        "    end; "
        "    return n; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);        // false on any leak / double-free / UAF
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// §29.7 + §28.4 — THE GUARD. `emitDisposals` is also reached over a
// FREE-STANDING disposal vector — one that is not on `disposeStack_` at all —
// by the iterator / async-iterator ABANDONMENT teardown, which runs inside the
// generated enumerator `Dispose` and still has work to finish after the pass:
// the remaining `IterCleanup` entries, the yielded value, the owned parameters
// and locals, and THE FRAME FREE. An unguarded post-pass check branches to
// `unwindBB()` from there and abandons all of it.
//
// These two rows pin that. Both abandon an iterator (`break` after the first
// element) whose own `using var`/`await using var` destructor throws — the one
// shape that makes the abandonment teardown itself raise.
//
// MEASURED, three ways, per row (async / sync):
//   base 7f29b8ce ............... exit 41, LSan clean
//   #1882 without the guard ..... exit 41, LEAKS 88 B / 48 B
//   #1882 with the guard ........ exit 41, LSan clean
// So `r.ran` is the load-bearing oracle here and the exit code is not: the
// fault is swallowed on base and still swallowed after this change (§29.7 is
// not one of the disposal points §28.4 or these two issues name — that gap is
// tracked separately). Both programs therefore return the SAME value from the
// fall-through and from the `catch` arm, so the rows keep working, unchanged,
// once §29.7's own propagation is fixed — deliberately not encoding today's
// swallow as a contract. 41 == 40 + the single element consumed, which is what
// proves the loop really ran and really was abandoned.
//
// WHY ONE-SHOT AND NOT LOOP-AMPLIFIED, against this project's usual rule: the
// loop form is unusable here because repeated abandonment has a SEPARATE,
// PRE-EXISTING frame leak — measured at 50 iterations as 1800 B (async) /
// 1200 B (sync) in 25 allocations, byte-identical on base 7f29b8ce and on this
// change — so an amplified row would be red on both sides and say nothing
// about the guard. The one-shot form is not vacuous here and was not assumed
// to be: LSan reports the abandoned frame deterministically, which is exactly
// what the middle line of the table above measures.
BEZEL_TEST(throwing_destructor_propagates_from_scope_end_1882,
           abandoned_async_iterator_teardown_does_not_leak_the_frame_spec_29_7) {
    auto r = runAotChecked(
        "module M; "
        "class Fault: Exception "
        "  public constructor() base(\"fault\"); end constructor; "
        "end Fault; "
        "class AsyncRes: IAsyncDisposable "
        "  public async method DisposeAsync(): Task "
        "  end DisposeAsync; "
        "  destructor throw new Fault(); end destructor; "
        "end AsyncRes; "
        "class C "
        "  public async static method Gen(): IAsyncEnumerable<int> "
        "    await using var r := new AsyncRes(); "
        "    yield return 1; yield return 2; yield return 3; "
        "  end Gen; "
        "end C; "
        "class C2 "
        "  public static async method Main(): Task<int> "
        "    var n := 0; "
        "    try "
        "      await for var x in C.Gen() do "
        "        n := n + x; "
        "        if x = 1 then break; end; "
        "      end; "
        "      return 40 + n; "
        "    catch var ex: Fault => do return 40 + n; end "
        "    end; "
        "  end Main; "
        "end C2;");
    BEZEL_EXPECT_TRUE(r.ran);        // false without the guard: 88 B frame leak
    BEZEL_EXPECT_EQ(r.exitCode, 41); // the loop ran and was abandoned at x == 1
}

BEZEL_TEST(throwing_destructor_propagates_from_scope_end_1882,
           abandoned_sync_iterator_teardown_does_not_leak_the_frame_spec_29_7) {
    auto r = runAotChecked(
        "module M; "
        "class Fault: Exception "
        "  public constructor() base(\"fault\"); end constructor; "
        "end Fault; "
        "class Res "
        "  destructor throw new Fault(); end destructor; "
        "end Res; "
        "class C "
        "  public static method Nums(): IEnumerable<int> "
        "    using var r := new Res(); "
        "    yield return 1; "
        "    yield return 2; "
        "    yield return 3; "
        "  end Nums; "
        "end C; "
        "class C2 "
        "  public static method Main(): int "
        "    var n := 0; "
        "    try "
        "      for var x in C.Nums() do "
        "        n := n + x; "
        "        if x = 1 then break; end; "
        "      end; "
        "      return 40 + n; "
        "    catch var ex: Fault => do return 40 + n; end "
        "    end; "
        "  end Main; "
        "end C2;");
    BEZEL_EXPECT_TRUE(r.ran);        // false without the guard: 48 B frame leak
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// CONTROL (unchanged by the fix): non-throwing destructors on the scope-end
// path. Pins that the added pending check is a no-op on the ordinary path — no
// spurious unwind, every iteration's disposals still run exactly once, and the
// loop-amplified ASan oracle stays clean.
BEZEL_TEST(throwing_destructor_propagates_from_scope_end_1882,
           non_throwing_scope_end_disposal_unchanged) {
    auto r = runAotChecked(
        "module M; "
        "class R "
        "  public static n: int := 0; "
        "  destructor R.n := R.n + 1; end destructor; "
        "end R; "
        "class C "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<100 do "
        "      using var a := new R(); "
        "      using var b := new R(); "
        "      var s := \"x\" + i.ToString(); "
        "      total := total + s.Length; "
        "    end; "
        "    return R.n; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}
