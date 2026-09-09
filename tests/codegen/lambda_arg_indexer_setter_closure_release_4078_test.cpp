// Codegen — issue #4078 follow-up (spec §28.14 "Call-bounded function
// parameters", §23.4 "Indexers", §27.5 "Closures", §28.9 "Runtime-managed
// types", v0.294): the indexer SETTER path `box[lambda() => new SomeOwned()] := v`
// in `lowerIndexerSet` (LlvmBackend_Assign.cpp ~2596) leaked the lambda's
// 40-byte closure cell. #4078's earlier fix (504abbd) wired
// `releaseIndexArgClosureProducers` into the four GETTER paths of
// `lowerIndexerGet` and into `emitClosureCall` (the function-VALUE call
// path), but left every setter path untouched. `lowerIndexerGet` and
// `lowerIndexerSet` are sibling emitters that share the §28.14 call-bounded
// contract — the §23.5-forbidden `ref`/`out`/`owned` on indexer parameters
// applies identically to BOTH halves of the access — so a closure-producer
// INDEX argument is provably the caller's sole reference once the SETTER
// returns, just as it is after the getter. The `releaseIndexArgStringProducers`
// helper already had this insight (it is called on setter paths 2, 4, and 5
// below) — its closure sibling `releaseIndexArgClosureProducers` was simply
// never wired in.
//
// MEASURED (HEAD pre-fix), Row 1 (class with non-virtual indexer, the
// inline-setter path — `lowerIndexerSet`'s path 5, line ~3074): 40 B / 1
// allocation per Main call, leaking the closure cell from `bz$M.C$Main`.
// The produced `Res` is consumed by the setter body
// (`set => do using var c := mk(); this.v := c.tag + value; end;` — the
// `using var c := mk()` matches the closure's `mk: () -> owned Res` target)
// so its `Res.live==0` is unaffected; the leak is the 40-byte closure cell
// itself.
//
// MEASURED (HEAD pre-fix), Row 2 (class with virtual indexer, the
// class-vtable-setter path — `lowerIndexerSet`'s path 4, line ~3008): same
// 40 B / 1 alloc per Main call.
//
// The interface-dispatch path (path 3, line ~2819) was not tested: the
// baseline interface-call site in this revision's `scratch/probe_4078_iface.bzl`
// aborts on the naked `iface_itable` call before the indexer-setter machinery
// runs (an unrelated interface-shape #2832-style gap), so wiring the closure
// release into that path against this fix's intent is out of scope — the
// gate will exercise it independently.
//
// LOOP-AMPLIFICATION ([lsan-one-shot-leak-tests-pass-vacuously] discipline):
// every row runs 500 iterations so a per-iteration 40 B leak cannot pass
// vacuously as "process exit frees it" — 500 × 40 = 20000 B, well above
// LSan noise. The figure matches every other leak-pin in this tree
// (lambda_arg_call_bounded_closure_release_4078_test.cpp Row 2,
// argpass_string_refcount_test.cpp's expectValueAndLeakFree family).
//
// SHAPE-NOTE: every lambda argument is `lambda() => new Res(9)` (a CONSTANT
// payload, not `lambda() => new Res(j)`). The closure CELL leak this fix
// addresses is independent of any captured-variable leak; the LAMBDA
// CAPTURE path is a separate, in-flight defect (the captured box is not
// released at the same edge as the closure cell, by a separate decision
// in lowerLambda). Using a constant payload isolates THIS fix's scope and
// keeps the row's failure mode pinned to the closure-cell release.
//
// DIRECTION OF DANGER. The new release is armed with the existing
// `releaseIndexArgClosureProducers` helper's own gates (§28.14 +
// `isPlusOneClosureProducer` + the `exprValueOverride_` §22.3
// `ref`/`out`-pinned-exclusion) — every clause fails closed (the safe
// direction is a leak, not a UAF). Every row uses `runAotChecked`
// (generated-code ASan with LSan on), so a double-free or use-after-free
// fails the row before the printed value is compared, and the row's
// printed value is itself an independent oracle on whether the setter
// actually ran.
//
// per §28.14, §27.5, §28.9, §23.4, §23.5 (issue #4078 follow-up, v0.294)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
// runAotChecked runs the program under generated-code ASan WITH LeakSanitizer
// on: a leak, a double free or a use-after-free fails the row before the
// printed exit value is compared.
constexpr const char* kRes =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "end Res; ";
}  // namespace

// ── Row 1 (issue repro #4078 follow-up / §23.4 + §28.14): a CLASS indexer
// SETTER `box[mk: () -> owned Res]: int` invoked with an inline-lambda
// INDEX ARGUMENT on the LHS of `box[k] := v`. The lambda `lambda() =>
// new Res(9)` is a +1 closure producer whose closure cell is the only
// reference once the setter returns (the setter body calls `mk()` via
// `using var c := mk(); this.v := c.tag + value;` and never stored the
// callback — §28.14 verified by `MethodOwnership::callBoundedScan`).
// Pre-fix this leaked 40 B per Main call from `bz$M.C$Main`. The fix
// calls `releaseIndexArgClosureProducers(ix, ps, argVals, …)` after the
// setter body has returned (line ~3232 of LlvmBackend_Assign.cpp, in the
// inline-setter path — `lowerIndexerSet`'s path 5), gated on
// `!diverged_`. Loop-amplified to 500 iterations (500 × 40 = 20000 B,
// unambiguous).
BEZEL_TEST(lambda_arg_indexer_setter_closure_release_4078,
           indexer_inline_setter_with_inline_lambda_loop_500_no_leak_23_4) {
    std::string src = std::string(kRes) +
        "class Box "
        "  public v: int := 0; "
        "  public property [mk: () -> owned Res]: int "
        "    get => do using var c := mk(); return c.tag; end; "
        "    set => do using var c := mk(); this.v := c.tag + value; end; "
        "  end; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(); "
        "    var acc := 0; "
        "    for var j in 0..<500 do "
        "      box[lambda() => new Res(9)] := j; "
        "      acc := acc + box.v; "
        "    end; "
        "    Console.WriteLine(\"acc=\" + acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    if (!r.ran) { BEZEL_EXPECT_TRUE(false); return; }
    // ASan/LSan clean ⇒ exit code is Main's return (0). acc = sum_{j=0..499}
    // (9 + j) = 500 * 9 + 499*500/2 = 4500 + 124750 = 129250.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "acc=129250\n");
}

// ── Row 2 (§23.4 + §28.14): a CLASS with a VIRTUAL indexer setter (a
// vtable-dispatched setter — `lowerIndexerSet`'s path 4, line ~3008).
// This is the SIBLING path to the inline setter (Row 1): the setter
// dispatch goes through `vt[propSetSlot_]` rather than being inlined.
// Pre-fix this also leaked the closure cell per call (same 40 B / 1 alloc
// measurement). The fix wires `releaseIndexArgClosureProducers(ix, ps,
// idxArgVals)` after the setter call (line ~3056 area), gated on
// `!diverged_`. Loop-amplified to 500 iterations to match every other
// leak-pin in this tree. The setter body reuses Row 1's shape: `using
// var c := mk(); this.v := c.tag + value;` so the produced `Res` is
// consumed correctly (the leak was only the closure cell).
BEZEL_TEST(lambda_arg_indexer_setter_closure_release_4078,
           indexer_vtable_setter_with_inline_lambda_loop_500_no_leak_23_4) {
    std::string src = std::string(kRes) +
        "class Box "
        "  public v: int := 0; "
        "  public virtual property [mk: () -> owned Res]: int "
        "    get => do using var c := mk(); return c.tag; end; "
        "    set => do using var c := mk(); this.v := c.tag + value; end; "
        "  end; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var box: Box := new Box(); "
        "    var acc := 0; "
        "    for var j in 0..<500 do "
        "      box[lambda() => new Res(9)] := j; "
        "      acc := acc + box.v; "
        "    end; "
        "    Console.WriteLine(\"acc=\" + acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    if (!r.ran) { BEZEL_EXPECT_TRUE(false); return; }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "acc=129250\n");
}

// ── Row 3 (§28.14 / §23.4): the SIMPLEST reproducer — a non-owned
// lambda returning a literal `int`. This is the SETTER twin of the
// existing Row 3 (`invoked_function_value_with_int_lambda_loop_500_no_leak_28_14`)
// in lambda_arg_call_bounded_closure_release_4078_test.cpp: the
// "fresh-owned" framing was misleading (#4078's framing, corrected by
// the fix — the leak is the closure CELL itself, not anything the body
// produces), so an `int`-returning lambda exhibits the SAME per-iteration
// 40 B leak through the SETTER path. Pre-fix: 20000 B / 500 allocs.
// Post-fix: 0 B (verified with runAotChecked ASan/LSan). exit value
// truncated to the 1-byte exit code from runAot — `acc` printed via
// stdout, then a 0 from Main.
BEZEL_TEST(lambda_arg_indexer_setter_closure_release_4078,
           indexer_inline_setter_with_int_lambda_loop_500_no_leak_23_4) {
    std::string src =
        "module M; "
        "class Box "
        "  public v: int := 0; "
        "  public property [k: () -> int]: int "
        "    get => do return k(); end; "
        "    set => do this.v := k() + value; end; "
        "  end; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(); "
        "    var acc := 0; "
        "    for var j in 0..<500 do "
        "      box[lambda() => 42] := j; "
        "      acc := acc + box.v; "
        "    end; "
        "    Console.WriteLine(\"acc=\" + acc.ToString()); "
        "    Console.WriteLine(\"v=\" + box.v.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    if (!r.ran) { BEZEL_EXPECT_TRUE(false); return; }
    // 500 iterations, each sets box.v = 42 + j where j runs 0..499.
    // acc = sum_{j=0..499} (42 + j) = 500*42 + 499*500/2 = 21000 + 124750 = 145750.
    // box.v after the loop is 42 + 499 = 541.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "acc=145750\nv=541\n");
}
