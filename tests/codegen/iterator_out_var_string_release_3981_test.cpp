// Spec-derived regression tests for issue #3981 — a `string`-typed `out var`
// recipient declared in an ITERATOR body (a method whose body contains `yield
// return`, backed by `IEnumerable<T>`) was never released: both allocations
// (the `bz_string` header and its payload) leaked, per iterator execution.
// Sibling of issue #3937 (the destructor-body venue for the same fault
// family) — same measured shape (20500 B / 1000 allocations at 500
// iterations), but a DIFFERENT mechanism: #3937 was `registerOutVarString-
// Release`'s stale "gate 5" (`emittingDisposerBody_`); this issue is about the
// recipient's storage never surviving a real suspend at all.
//
// THE MECHANISM (established by inspecting `BEZEL_DUMP_IR=1` output for the
// issue's own repro, HEAD before this fix). An iterator body lowers into a
// `MoveNext`-style state-machine function, called ONCE PER ADVANCE — each
// call is a fresh activation with its own fresh stack. `ensureOutVarSlots`
// (`LlvmBackend_Calls2.cpp`) allocates the `out var` recipient's storage with
// a bare `entryAlloca` — a stack slot local to ONE such activation — and (for
// a non-nullable `string` recipient) registers a SCOPE-END release into
// `disposeStack_`. Fix #2817 (v0.240) already solved the identical problem
// for a plain `var` string LOCAL by giving every `ast::LocalDecl` a
// persistent STATE-MACHINE FRAME FIELD (`collectSuspendLocals`) so the
// per-suspend spill/restore (`smAllocaToField_`, consulted by the yield
// handler) carries its value from the activation that BINDS it to the
// (possibly later) activation whose scope-end drain RELEASES it — exactly the
// "one bind, one release, per element" activation model #2817's own comment
// documents. An inline `out var` recipient never got the same treatment: sema
// mints its `LocalSymbol` with `decl` pointing at the ARGUMENT's own
// `IdentifierExpr` (`ExprTypecheck_Exprs.cpp`), not a `LocalDecl`, so
// `collectSuspendLocals`'s statement-KIND switch structurally cannot see it,
// no matter how many arms it grows (`outVarStringReleaseEligible`'s own
// comment, `LlvmBackend_Setup.cpp`, makes the identical observation about why
// it is a SEPARATE on-demand walk rather than a `collectStringReleaseEligible`
// arm). Without a frame field, the very next activation resets the
// recipient's alloca to its null default BEFORE the deferred release
// (registered by an EARLIER activation) ever runs; the release then fires
// against null — a documented no-op — and the real string pointer that
// activation actually produced is unreachable: leaked, not corrupted.
//
// THE FIX. `collectOutVarSuspendLocals` (LlvmBackend_Iterators.cpp) is a new
// companion pre-pass — a full statement + expression descent (an inline
// `out var` can be buried at any expression position a CallExpr/NewExpr
// reaches, not just the handful of statement shapes `collectSuspendLocals`
// switches on) — that reserves a PTR-WIDE frame field for every inline
// `out var` recipient found, called from `buildStateMachineFrame` (the ONE
// frame builder shared by both the sync/async iterator path and the async-
// method path, per the file's own "Phase 2a" header comment) right after
// `collectSuspendLocals`. `registerOutVarStringRelease` and
// `registerCtorOutVarStringRelease` (`LlvmBackend_Calls2.cpp`) each now call
// `mapSuspendLocalSlot` — the SAME wiring #2132 already established for a
// plain suspend-live local — right after their own gate 2 has confirmed the
// recipient's FORMAL is non-nullable `string` (so `ptr`-width, matching the
// field width the collector reserved; see the collector's own comment for why
// over-collecting non-string recipients is inert rather than unsafe: a
// reserved-but-unwired field is untouched by the spill/restore loop, so a
// wider-than-pointer value-typed out-var recipient elsewhere in the codebase
// can never suffer a mismatched-width overflow from this fix).
//
// Because `buildStateMachineFrame` is shared, this fix closes the identical
// gap for an ASYNC method with a real `await` between the out-var call and
// its use/scope-end too (the ASYNC_ACROSS_AWAIT test below) — a case the
// issue's own async CONTROL happened not to exercise (its repro never
// actually suspended, so it was clean by accident: no frame field was needed
// because bind and release landed in the SAME activation either way).
//
// SIBLING FORM FOUND WHILE FIXING THIS (per this project's "check sibling
// recipient types/forms" discipline): `out using var` — the scope-bound
// owned-recipient form (§22.4/§28.11/§28.13, e.g. `OwningStack<T>.TryPop(out
// using var item)`) — allocates its slot identically
// (`entryAlloca(ptrTy())`, unconditionally) and registers an unconditional
// `dynamicDispose`/`sharedHandle` disposal exactly the way `out var`
// registers a `stringRelease` one. Measured leaking in the SAME iterator
// venue pre-fix (8000 B / 500 allocations, `OUT_USING_VAR` test below); fixed
// in the SAME commit, since the mechanism and the safety argument
// (unconditional `ptr`-width) are identical — see `OutVarSlotCollector`'s own
// comment (LlvmBackend_Iterators.cpp) for why both forms are collected
// together. The retired `out owned var` spelling (no parser emits it since
// v0.289) and the pre-declared `out owned`/bare-`out` forms (which bind an
// EXISTING LocalDecl's slot, already frame-backed) are out of scope — neither
// is reachable from current source.
//
// per §22.3, §22.4, §28.9, §28.11, §28.13, §29.2, §29.7, §30.1, §30.2, v0.294

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"   // runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// ASan + LSan oracle: a leak / double-free / UAF report at exit surfaces as
// ran=false, which this flips to the -999 sentinel so a plain int comparison
// serves as the assertion.
int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// -----------------------------------------------------------------------------
// Issue #3981's own repro, loop-amplified: 500 executions of an iterator whose
// body binds a `string` `out var` recipient via a plain static method call and
// reads it (`nm.Length`) in the SAME `yield return` that then suspends the
// MoveNext state machine — the recipient's scope-end release is therefore
// necessarily deferred to the NEXT activation (there is nothing after the
// `yield return` in the body). "abc" + "-tail" = "abc-tail" is 8 chars, so a
// correct run totals 500*8 = 4000; the test asserts that INTERNALLY (`return
// (if t = 4000 then 0 else 1)`) rather than via the exit code directly, which
// truncates to 8 bits (LANE_RULES §11). Pre-fix: LSan reports 20500 B / 1000
// allocations (measured — the issue's own number). Post-fix: clean, exit 0.
// per §22.3, §22.4, §28.9, §29.2, §29.7
// -----------------------------------------------------------------------------
BEZEL_TEST(iterator_out_var_string_release_3981,
           single_call_out_var_string_recipient_in_iterator_is_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class W "
        "  public static method Make(s: string, out n: string): bool "
        "    n := s + \"-tail\"; return true; "
        "  end Make; "
        "end W; "
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    var okv := W.Make(\"abc\", out var nm); "
        "    _ := okv; "
        "    yield return nm.Length; "
        "  end Gen; "
        "  public static method Run(): int "
        "    var total := 0; "
        "    for var v in C.Gen() do total := total + v; end; "
        "    return total; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..500 do t := t + C.Run(); end; "
        "    return (if t = 4000 then 0 else 1); "
        "  end Main; end C;"), 0);
}

// -----------------------------------------------------------------------------
// DOUBLE-FREE HUNT — a LOOP inside the ITERATOR body reuses the recipient's
// slot across MULTIPLE suspend/resume cycles: each of Gen()'s 5 `yield
// return`s inside its own `while` binds a fresh `out var nm` from the SAME
// call site, so the SAME frame field this fix reserves is spilled, released,
// and re-bound five times per Gen() execution — the #2629-shaped
// `Disposal::nullSlotAfterRelease` case, now exercised across a REAL suspend
// for the first time. "x" + "-tail" = "x-tail" is 6 chars; 5 yields per
// Run() x 500 Run()s = 500*5*6 = 15000. If the slot were not nulled after
// release, the NEXT bind's callee-side load in `lowerStringRefParamStore`
// would read already-freed memory — a double-release ASan reports rather than
// tolerates. LSan-clean AND value-correct is the whole bar.
// per §22.3, §22.4, §28.9, §29.2, §29.7
// -----------------------------------------------------------------------------
BEZEL_TEST(iterator_out_var_string_release_3981,
           loop_reused_out_var_slot_across_real_suspends_is_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class W "
        "  public static method Make(s: string, out n: string): bool "
        "    n := s + \"-tail\"; return true; "
        "  end Make; "
        "end W; "
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    var i := 0; "
        "    while i < 5 do "
        "      var okv := W.Make(\"x\", out var nm); "
        "      _ := okv; "
        "      yield return nm.Length; "
        "      i := i + 1; "
        "    end; "
        "  end Gen; "
        "  public static method Run(): int "
        "    var total := 0; "
        "    for var v in C.Gen() do total := total + v; end; "
        "    return total; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..500 do t := t + C.Run(); end; "
        "    return (if t = 15000 then 0 else 1); "
        "  end Main; end C;"), 0);
}

// -----------------------------------------------------------------------------
// CONSTRUCTOR SIBLING — `registerCtorOutVarStringRelease` (the `new T(out var
// n)` form, fix #3305) shares the exact same activation-crossing gap: a
// `string` `out var` recipient of a CONSTRUCTOR call, declared directly in an
// iterator body, also leaked pre-fix for the identical reason. `W`'s
// constructor writes through the hidden out-reference exactly as the static
// method does.
// per §22.3, §22.4, §28.9, §29.2, §29.7
// -----------------------------------------------------------------------------
BEZEL_TEST(iterator_out_var_string_release_3981,
           ctor_out_var_string_recipient_in_iterator_is_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class W "
        "  public constructor(s: string, out n: string) "
        "    n := s + \"-tail\"; "
        "  end constructor; "
        "end W; "
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    var w := new W(\"abc\", out var nm); "
        "    _ := w; "
        "    yield return nm.Length; "
        "  end Gen; "
        "  public static method Run(): int "
        "    var total := 0; "
        "    for var v in C.Gen() do total := total + v; end; "
        "    return total; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..500 do t := t + C.Run(); end; "
        "    return (if t = 4000 then 0 else 1); "
        "  end Main; end C;"), 0);
}

// -----------------------------------------------------------------------------
// SIBLING FORM — `out using var`, the scope-bound OWNED-recipient form
// (§22.4/§28.11/§28.13), inside the SAME iterator venue: `OwningStack<Box>.
// TryPop(out using var c)` binds a moved-out `Box` and registers an
// unconditional `dynamicDispose` — the class-object analogue of `out var`'s
// `stringRelease`, allocated the identical way (`entryAlloca(ptrTy())`,
// unconditionally) and therefore exposed to the identical activation-
// crossing gap. `Box`'s destructor prints "dispose" so exactly-once disposal
// is independently observable in `stdoutText`, not just LSan-clean. Pre-fix:
// 8000 B / 500 allocations (16 bytes/Box: header + one `int` field, measured
// independently of the issue's own `string` figure). Post-fix: clean, exactly
// 500 "dispose" lines, exit 0.
// per §22.4, §28.9, §28.11, §28.13, §29.2, §29.7
// -----------------------------------------------------------------------------
BEZEL_TEST(iterator_out_var_string_release_3981,
           out_using_var_owned_recipient_in_iterator_is_leak_free) {
    JitResult r = runAotChecked(
        "module M; import System; "
        "class Box "
        "  public v: int; "
        "  public constructor(v: int) this.v := v; end constructor; "
        "  destructor Console.WriteLine(\"dispose\"); end destructor; "
        "end Box; "
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    using var s := new OwningStack<Box>(); "
        "    s.Push(new Box(7)); "
        "    if s.TryPop(out using var c) then "
        "      yield return c!.v; "
        "    end; "
        "  end Gen; "
        "  public static method Run(): int "
        "    var total := 0; "
        "    for var v in C.Gen() do total := total + v; end; "
        "    return total; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..500 do t := t + C.Run(); end; "
        "    return (if t = 3500 then 0 else 1); "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    std::size_t n = 0, pos = 0;
    while ((pos = r.stdoutText.find("dispose", pos)) != std::string::npos) {
        ++n;
        pos += 7;
    }
    BEZEL_EXPECT_EQ(static_cast<int>(n), 500);
}

// -----------------------------------------------------------------------------
// ASYNC SIBLING, ACROSS A REAL SUSPEND — `buildStateMachineFrame` is the ONE
// frame builder both the iterator path and the async-method path share (the
// file's own "Phase 2a" header comment), so this fix closes the identical gap
// for an ASYNC method too, whenever an `await` actually separates the out-var
// call from its use — unlike the issue's own async CONTROL (ported below),
// which never suspended and so was clean by accident. Here `nm` is bound
// BEFORE `await Task.Delay(0);` and READ (`nm.Length`) after resuming — a
// stronger check than a deferred release alone: this proves the recipient's
// VALUE, not just a disposal obligation, survives the suspend, using the same
// generic per-suspend spill/restore machinery an ordinary suspend-live local
// already relies on.
// per §22.3, §22.4, §28.9, §30.1, §30.2, §30.7
// -----------------------------------------------------------------------------
BEZEL_TEST(iterator_out_var_string_release_3981,
           async_out_var_string_recipient_across_real_suspend_is_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class W "
        "  public static method Make(s: string, out n: string): bool "
        "    n := s + \"-tail\"; return true; "
        "  end Make; "
        "end W; "
        "class C "
        "  public static async method Compute(): Task<int> "
        "    var okv := W.Make(\"abc\", out var nm); "
        "    _ := okv; "
        "    await Task.Delay(0); "
        "    return nm.Length; "
        "  end Compute; "
        "  public static async method Main(): Task<int> "
        "    var t := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      var r := await C.Compute(); "
        "      t := t + r; "
        "      i := i + 1; "
        "    end; "
        "    return (if t = 4000 then 0 else 1); "
        "  end Main; end C;"), 0);
}

// -----------------------------------------------------------------------------
// CONTROL — LAMBDA BODY. Ported from the issue's own sweep: a lambda body
// compiles as its own straight-line function (no suspend/frame machinery),
// so it was ALREADY clean pre-fix; pinned here so a future regression that
// makes this fix's collector wrongly descend into a lambda body (rather than
// stopping at it, as `OutVarSlotCollector::expr` deliberately does) is caught
// by contrast with the iterator tests above.
// per §22.3, §22.4, §28.9, §27.5
// -----------------------------------------------------------------------------
BEZEL_TEST(iterator_out_var_string_release_3981,
           control_lambda_body_out_var_string_recipient_is_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class W "
        "  public static method Make(s: string, out n: string): bool "
        "    n := s + \"-tail\"; return true; "
        "  end Make; "
        "end W; "
        "class C "
        "  public static method Run(): int "
        "    var f := lambda(): int => do "
        "      var okv := W.Make(\"abc\", out var nm); "
        "      _ := okv; "
        "      return nm.Length; "
        "    end; "
        "    return f(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..500 do t := t + C.Run(); end; "
        "    return (if t = 4000 then 0 else 1); "
        "  end Main; end C;"), 0);
}

// -----------------------------------------------------------------------------
// CONTROL — ASYNC BODY, NO SUSPENSION. The issue's own async control, ported
// verbatim in shape: an `async method` whose body never actually awaits
// anything runs straight-through in ONE activation, so bind and release land
// together regardless of a frame field. Pinned alongside the real-suspend
// test above so the two together demonstrate the fix is not merely
// coincidentally correct for async — it is correct WITH a real suspend and
// (already, before this fix) correct WITHOUT one.
// per §22.3, §22.4, §28.9, §30.1
// -----------------------------------------------------------------------------
BEZEL_TEST(iterator_out_var_string_release_3981,
           control_async_no_suspend_out_var_string_recipient_is_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class W "
        "  public static method Make(s: string, out n: string): bool "
        "    n := s + \"-tail\"; return true; "
        "  end Make; "
        "end W; "
        "class C "
        "  public static async method Compute(): Task<int> "
        "    var okv := W.Make(\"abc\", out var nm); "
        "    _ := okv; "
        "    return nm.Length; "
        "  end Compute; "
        "  public static async method Main(): Task<int> "
        "    var t := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      var r := await C.Compute(); "
        "      t := t + r; "
        "      i := i + 1; "
        "    end; "
        "    return (if t = 4000 then 0 else 1); "
        "  end Main; end C;"), 0);
}
