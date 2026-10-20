// Codegen / leak — a LAMBDA LITERAL (or method reference) passed DIRECTLY as a
// PLAIN function-typed method argument was never released: the closure cell and
// its env block leaked once per call. Issue #1073.
//
// Spec: §28.14 (scoped callback parameters — "The call-bounded default"),
// §28.9 (runtime-managed types / closure-cell reclaim), §27.5 (closures and
// captures), §27.6 (method references), §28.3 (exception-path discharge),
// §29.3 (iterator methods — the deferred-activation frame). Spec revision
// v0.226.
//
// THE BUG. A lambda literal passed directly as a method argument is a `+1`
// closure producer: `lowerLambda` callocs a fresh cell at rc = 1 and codegen
// owns that single reference end to end. The callee parameter is a BORROW, so
// nothing on the callee side ever drops it, and codegen emitted no caller-side
// release either. LSan on the issue's exact repro, verified on this worktree's
// base (0280d4c):
//
//   Direct leak of 40 byte(s) in 1 object(s)   <- the closure cell
//   Indirect leak of 8 byte(s) in 1 object(s)  <- its capture env block
//   SUMMARY: AddressSanitizer: 48 byte(s) leaked in 2 allocation(s).
//
// WHY THE CALLER MAY DROP IT (§28.14, "The call-bounded default"): "A
// function-typed parameter declared *without* `scoped` is call-bounded: the
// callee receives the callback for the duration of the call only … it may not
// be stored in a field of any object, stored into any collection that is itself
// reachable beyond the call, returned, captured by a closure that itself
// escapes the callee, or forwarded into a `scoped` parameter position. These
// obligations are compiler-verified for user-authored callees." That
// verification is `MethodOwnership::callBoundedScan`
// (src/ownership/Ownership_Checks.cpp), which enrols exactly the empty-modifier,
// function-typed parameters and rejects every one of those sinks. So for that
// parameter shape the callee provably holds no reference once the call returns.
//
// THE FIX (src/codegen/LlvmBackend_Calls2.cpp, `Emitter::lowerCall`; the
// unwind-edge cleanup in src/codegen/LlvmBackend_Disposal.cpp,
// `Emitter::closureArgUnwindCleanup`). After a call to a user-authored,
// non-async, non-iterator callee whose declared return does not announce a
// deferred-activation frame, release each `+1` closure producer passed into a
// plain (empty-modifier) SOURCE-WRITTEN function-typed parameter — on the
// normal-return edge, and, via the installed unwind target, on the
// exception/propagate edge as well.
//
// THE #1921 / #2248 GUARD (case H below). A structurally identical sibling fix
// was reverted on 2026-07-29 as a USE-AFTER-FREE: it keyed its exclusion on the
// callee's *declared return type* resolving to `TypeRef::Kind::Function`, but a
// generic method declared `: T` resolves to `Kind::Named` (there is no TypeParam
// kind), so under `T := () -> int` the exclusion stopped firing and the release
// freed a cell the callee had not retained. This fix's arming predicate is built
// only from SOURCE-WRITTEN declaration shapes, which monomorphization never
// rewrites: the PARAMETER must be spelled as a function type
// (`ast::TypeKind::Function`), so a bare-`T` parameter — `Kind::Named` in the
// AST whatever `T` becomes — can never arm the release at any instantiation.
// Case H is that exact shape; with the parameter clause deliberately removed
// from the guard it reproduces the reverted commit's crash verbatim
// (`ERROR: AddressSanitizer: heap-use-after-free … freed by … bz_closure_release
//  … in main`), and with the guard in place it runs clean.
//
// OBSERVABILITY. `runAotChecked` links and runs the generated code under
// ASan/LSan, so a leak, double-free, or use-after-free flips `r.ran` to false;
// the exit code independently pins the computed value, so a premature free
// would corrupt it too. Cases H and I use `runAotCheckedLenient` (#2214: LSan
// off, generated-code ASan still on) because those are exactly the shapes the
// guard must NOT touch: they still carry the pre-existing #1073 leak (the safe
// direction), while a wrongly-armed release would surface as the ASan
// use-after-free the lenient helper still catches.
//
// FAIL-WITHOUT / PASS-WITH. Cases A–G leak on base (A: 48 bytes; B: 2400 bytes
// over 50 iterations; C/D/F/G: 40 bytes each) and are LSan-clean with the fix.
// Case E (#1864's sync-iterator release) and cases H/I are clean both before
// and after — they pin that the fix adds no double release and does not arm on
// the shapes that legitimately retain the callback.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 10 checked rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics, and then RETURNED from the row on
// it. That is an ABSORBING site by
// scripts/check_toolchain_skip_surface.py's own definition, and it is a silent
// vacuous pass: on a box whose clang cannot link the ASan binary, all
// 10 rows returned WITHOUT asserting and the suite reported a clean PASS
// having compiled and run nothing. MEASURED on the pre-change tree with the
// harness's own knob (`BEZEL_AOT_FORCE_NO_TOOLCHAIN=1`): every row green.
//
// Every one of those guards becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position: the
// skip is now LOUD — the row FAILS — instead of being absorbed by a guard
// that reports a clean pass. The lenient legs' assertions additionally
// now RUN UNCONDITIONALLY rather than being skipped, which is the other half of
// the strengthening. Every substantive assertion in every row (r.ran and the
// exitCode pins) stays exactly where it was.

}  // namespace

// ===========================================================================
// A — the issue's exact repro. §28.14 call-bounded parameter, capture-bearing
// lambda literal passed directly. 40-byte cell + 8-byte env leaked on base.
// ===========================================================================
BEZEL_TEST(codegen_1073_closure_arg, lambda_literal_arg_released_spec_28_14) {
    auto r = runAotChecked(
        "module M; "
        "class Box "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Box; "
        "class Sink "
        "  public method Take(h: () -> ()) h(); end Take; "
        "end Sink; "
        "class C "
        "  public static method Main(): int "
        "    using var box := new Box(); "
        "    using var s := new Sink(); "
        "    s.Take(lambda () => do box.Add(1); end); "
        "    return box.V; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ===========================================================================
// B — 50 evaluations of the same call site. On base the leak scales with the
// iteration count (2400 bytes over 50 allocations), so this is the unmistakable
// fail-without signal; with the fix every iteration's temporary is reclaimed.
//
// ── ORACLE NARROWED BY fix #3484 (§27.5, v0.283):
// `runAotChecked` → `runAotCheckedLenient` (ASan on, LeakSanitizer off).
//
// An earlier revision of this fix dropped to plain `runAot`, which throws away
// the generated-code AddressSanitizer as well — and only the LEAK half of the
// oracle is what this program can no longer satisfy. That matters here more than
// most places: the neighbouring defect class in this file's family (#2691) is a
// heap-use-after-free, which ASan catches and `runAot` would not. #2214 added
// `runAotCheckedLenient` for exactly this "the program incidentally leaks, but
// the memory net must stay on" idiom, and this file already uses it three times.
//
// This program leaks EIGHT BYTES — the §27.5 capture cell holding the `Box`
// reference — and always has. It is not #1073's leak (that one is the 50 closure
// temporaries, 2400 bytes) and no fix has ever reclaimed it: a capture site
// INSIDE A LOOP takes boxLocalSlot's guarded path, and `lowerLambda`'s
// scope-end admission excludes that shape on purpose (`loopStack_.empty()` —
// see #3435 / #3403, where the exclusion is the documented direction-of-danger
// choice). Amplified to 500 calls the shape reports 4000 bytes in 500
// allocations, MEASURED IDENTICALLY on the pre-#3484 and post-#3484 compilers.
//
// The row nevertheless passed under LeakSanitizer, because ONE execution leaves
// the cell pointer in a dead stack frame that LSan's conservative root scan
// still reads — the standard one-shot-LSan vacuity. #3484 changes the
// instructions around the two reads of that holder (see below), which is enough
// to end the accident; the leak it reveals is not one this fix caused.
//
// #3484 also makes this very program well-defined. Its `while i < n` reads the
// captured `box` AFTER the loop — through the cell — and its `using var box`
// disposal does the same, so on a ZERO-TRIP loop both dereference the holder the
// capture site never filled. Measured on the pre-fix compiler with the bound
// passed in as 0: "AddressSanitizer: SEGV on unknown address 0x000000000000 in
// bz$M.C$Step". Post-fix that program returns 0 cleanly.
//
// So the row keeps its program, its VALUE assertion (50 `Add` calls did happen)
// and its use-after-free net, and gives up only the leak assertion that was never
// true. #1073's own leak contract stays pinned by row A above, which is still
// `runAotChecked` and still green. The 8-byte cell is filed separately (#3498).
// ===========================================================================
BEZEL_TEST(codegen_1073_closure_arg, lambda_literal_arg_in_loop_released_spec_28_14) {
    auto r = runAotCheckedLenient(
        "module M; "
        "class Box "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Box; "
        "class Sink "
        "  public method Take(h: () -> ()) h(); end Take; "
        "end Sink; "
        "class C "
        "  public static method Main(): int "
        "    using var box := new Box(); "
        "    using var s := new Sink(); "
        "    var i := 0; "
        "    while i < 50 do "
        "      s.Take(lambda () => do box.Add(1); end); "
        "      i := i + 1; "
        "    end; "
        "    return box.V; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 50);
}

// ===========================================================================
// C — §27.6 method reference. `makeMethodThunk` mints the same rc=1 cell as a
// lambda literal, so the same release must arm (isPlusOneClosureProducer covers
// both). 40 bytes leaked on base.
// ===========================================================================
BEZEL_TEST(codegen_1073_closure_arg, method_reference_arg_released_spec_27_6) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static method Five(): int return 5; end Five; "
        "  public static method Apply(f: () -> int): int "
        "    return f(); "
        "  end Apply; "
        "  public static method Main(): int "
        "    return C.Apply(C.Five); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ===========================================================================
// D — §28.3 exception-path discharge: the callee PROPAGATES after invoking the
// callback. The normal-edge release is branched past, so without the unwind
// cleanup the temporary leaked on the caught path (40 bytes on base, and also
// with a normal-edge-only fix). Two `using var` scopes and the catch arm are
// exercised on the way out, pinning that the cleanup composes with the ordinary
// unwind disposals rather than replacing them.
// ===========================================================================
BEZEL_TEST(codegen_1073_closure_arg, released_on_unwind_edge_spec_28_3) {
    auto r = runAotChecked(
        "module M; "
        "class Res "
        "  public V: int := 0; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Inner(f: () -> int): int "
        "    var v := f(); "
        "    throw new InvalidOperationException(\"inner\"); "
        "  end Inner; "
        "  public static method Mid(g: () -> int): int "
        "    using var r := new Res(); "
        "    return C.Inner(g); "
        "  end Mid; "
        "  public static method Main(): int "
        "    var n := 0; "
        "    try "
        "      using var r2 := new Res(); "
        "      n := C.Mid(lambda () => 3); "
        "    catch e: InvalidOperationException => "
        "      n := 9; "
        "    end; "
        "    return n; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 9);
}

// ===========================================================================
// D2 — the callee SWALLOWS the exception in its own try/catch and returns
// normally. The inner catch restores the enclosing unwind target, so control
// resumes on the normal edge and only the post-call release runs: a
// double-release guard for the unwind cleanup.
// ===========================================================================
BEZEL_TEST(codegen_1073_closure_arg, callee_swallowed_throw_no_double_release_spec_28_3) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static method Swallow(f: () -> int): int "
        "    var n := 0; "
        "    try "
        "      n := f(); "
        "      throw new InvalidOperationException(\"x\"); "
        "    catch e: InvalidOperationException => "
        "      n := n + 10; "
        "    end; "
        "    return n; "
        "  end Swallow; "
        "  public static method Main(): int "
        "    return C.Swallow(lambda () => 3); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 13);
}

// ===========================================================================
// E — §29.3 sync iterator callee. #1864 ALREADY releases this shape (the
// iterator frame retains its own +1 at construction); the new block must skip
// it or the cell would be released TWICE. Clean before and after the fix — a
// pure double-release guard.
// ===========================================================================
BEZEL_TEST(codegen_1073_closure_arg, sync_iterator_callee_no_double_release_spec_29_3) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static method Gen(n: int, f: (int) -> int): IEnumerable<int> "
        "    var i := 0; "
        "    while i < n do "
        "      yield return f(i); "
        "      i := i + 1; "
        "    end; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var v in C.Gen(4, lambda (x) => x * 2) do "
        "      total := total + v; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 12);  // 0 + 2 + 4 + 6
}

// ===========================================================================
// F — §28.14's admitted "purely local collection" sink: the callee stores the
// callback in a local Array and invokes it through the collection. The local
// collection holds no independent +1, so the caller's post-call release is the
// only drop — and must still be exactly one.
// ===========================================================================
BEZEL_TEST(codegen_1073_closure_arg, local_collection_sink_released_once_spec_28_14) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static method Run(f: () -> int): int "
        "    using var xs: Array<() -> int> := []; "
        "    xs.Add(f); "
        "    var g := f; "
        "    return xs[0]() + g(); "
        "  end Run; "
        "  public static method Main(): int "
        "    return C.Run(lambda () => 3); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}

// ===========================================================================
// G — interface-dispatched callee. The statically resolved declaration is the
// interface's (body-less) method; the executed body is the override. Every
// implementation of a plain function-typed parameter carries the same §28.14
// call-bounded obligation, so the release is sound through the dispatch.
// ===========================================================================
BEZEL_TEST(codegen_1073_closure_arg, interface_dispatch_arg_released_spec_28_14) {
    auto r = runAotChecked(
        "module M; "
        "interface ISink method Take(h: () -> int): int; end ISink; "
        "class Direct : ISink "
        "  public method Take(h: () -> int): int => h(); "
        "end Direct; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Direct(); "
        "    var s: ISink := d; "
        "    return s.Take(lambda () => 11); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 11);
}

// ===========================================================================
// H — THE #1921 / #2248 UAF GUARD. A generic method with a BARE TYPE PARAMETER
// parameter (`Keep(x: T)`) instantiated at a FUNCTION type, whose body stores
// the value into a `T?` field. Codegen does not recognise a `T?` field as a
// closure storage member, so it emits no retain: the caller's `+1` is the ONLY
// reference keeping the cell alive, and a call-site release would free a cell
// the field still points at. `var g := lambda () => 7;` right after the call
// makes that fatal deterministically — glibc's tcache hands the just-freed
// 40-byte cell straight back to the next 40-byte calloc, so the field would end
// up aliasing g's closure.
//
// Verified: with `p.type->kind != ast::TypeKind::Function` removed from the
// arming predicate, this program reproduces the reverted #1921 commit's crash
// verbatim — `ERROR: AddressSanitizer: heap-use-after-free … freed by thread T0
// here: … bz_closure_release … in main`. With the guard it runs clean and
// returns 49.
//
// runAotCheckedLenient (#2214): the program still carries the pre-existing
// #1073 leak in this shape — the guard deliberately does NOT arm here, and a
// leak is the safe direction — so LSan is off while generated-code ASan (which
// is what catches the UAF) stays on.
// ===========================================================================
BEZEL_TEST(codegen_1073_closure_arg, bare_type_parameter_sink_not_released_spec_28_14) {
    auto r = runAotCheckedLenient(
        "module M; "
        "class Box<T> "
        "  public F: T?; "
        "  public method Keep(x: T) "
        "    F := x; "
        "  end Keep; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<() -> int>(); "
        "    b.Keep(lambda () => 42); "
        "    var g := lambda () => 7; "
        "    return b.F!() + g(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 49);  // 42 + 7 — 14 or a crash means the cell was freed
}

// ===========================================================================
// I — §28.14 *Call-bounded × deferred activation*: the callee forwards its
// call-bounded predicate into a lazy query and RETURNS the frame under a
// deferred-activation return type. The frame carries the callback past the
// call, so the caller must NOT release at the call site; the declared-return
// clause of the guard is what excludes it. Lenient for the same reason as H —
// this shape keeps the pre-existing leak, and the ASan net still pins the
// no-UAF half.
// ===========================================================================
BEZEL_TEST(codegen_1073_closure_arg, deferred_activation_return_not_released_spec_28_14) {
    auto r = runAotCheckedLenient(
        "module M; "
        "class C "
        "  public static method Q(xs: Array<int>, pred: (int) -> bool): IEnumerable<int> "
        "    return xs.Where(pred); "
        "  end Q; "
        "  public static method Main(): int "
        "    using var xs: Array<int> := [1, 2, 3, 4]; "
        "    var total := 0; "
        "    for var v in C.Q(xs, lambda (x) => x > 2) do "
        "      total := total + v; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);  // 3 + 4
}
