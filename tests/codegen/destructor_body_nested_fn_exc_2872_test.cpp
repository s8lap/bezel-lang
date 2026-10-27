// fix #2872 / §28.4 "The `destructor`" + §27.3 "Lambda expressions" + §28.3,
// v0.240: emitting a SECOND llvm::Function while a destructor body is being
// lowered must not reference the disposer's entry-block exception load.
//
// §28.4: the compiler-generated `Dispose()` "1. Runs the destructor body, if
// any." — the body is an ordinary statement block, so it admits a lambda
// (§27.3) and a call to one of the object's own methods. Two such bodies
// emitted malformed IR and aborted the build with the LLVM verifier's own
// message instead of running:
//
//     Referring to an instruction in another function!
//       %21 = icmp ne ptr %19, %disposer.entry.exc
//
// `disposer.entry.exc` is the #1468/#1607 load of `bz_exc` placed in the
// DISPOSER function's entry block (LlvmBackend_Disposal.cpp). Lowering the
// destructor body can emit another function mid-flight — a lambda's closure
// body (`lowerLambda`), a lazily-built generic specialization
// (`buildSpecializedMethodFn`) — and neither emitter neutralised
// `emittingDisposerBody_`/`disposerEntryExc_`, so `emitPendingCheck` (after any
// call) and the destructor-body `return` restore built an `icmp`/`select`
// against the OUTER function's value from inside the inner one.
//
// These tests run the programs end to end: without the fix they do not compile
// at all (`ran == false` -> the -999 sentinel), with it they produce the
// spec-stated value. The final two tests are the CONTROLS for the behaviour the
// fix must not disturb: #1468/#1607's rule that a destructor running during an
// unwind (§28.4 "the remaining steps of the generated `Dispose()` still run")
// completes its body, its owned-field cascade and its base chain.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ===========================================================================
// Shape 1 — NESTED lambdas in a destructor body (the issue's first repro).
// The inner call `inner(x)` inside the OUTER lambda's body emits a
// pending-check while the emitter still held the disposer's entry-exc value.
// Generic owner: `Box<string>` also forces the specialized disposer path.
// ===========================================================================
BEZEL_TEST(destructor_body_nested_fn_2872, nested_lambdas_generic_owner_spec_28_4) {
    BEZEL_EXPECT_EQ(runFull(
        "module Main; "
        "public class Box<T> "
        "  public v: T; "
        "  public tag: int; "
        "  public static Trace: int; "
        "  destructor "
        "    var outer: (int) -> int := lambda(x: int) => do "
        "      var inner: (int) -> int := lambda(y: int) => y * 2; "
        "      return inner(x) + 1; "
        "    end; "
        "    Trace := 100 + outer(tag); "
        "  end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  do using var b := new Box<string>(); b.tag := 6; end; "
        "  return Box<string>.Trace; "
        "end Main; end C;"), 113);
}

// Same shape on a NON-generic owner: the defect is the nested lambda, not the
// specialization, so this must lower and run too.
BEZEL_TEST(destructor_body_nested_fn_2872, nested_lambdas_nongeneric_owner_spec_28_4) {
    BEZEL_EXPECT_EQ(runFull(
        "module Main; "
        "public class Box "
        "  public tag: int; "
        "  public static Trace: int; "
        "  destructor "
        "    var outer: (int) -> int := lambda(x: int) => do "
        "      var inner: (int) -> int := lambda(y: int) => y * 2; "
        "      return inner(x) + 1; "
        "    end; "
        "    Trace := 100 + outer(tag); "
        "  end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  do using var b := new Box(); b.tag := 6; end; "
        "  return Box.Trace; "
        "end Main; end C;"), 113);
}

// ===========================================================================
// Shape 2 — a SELF-METHOD CALL in a destructor body (the issue's second repro).
// `Helper(tag)` on a generic owner is lowered by buildSpecializedMethodFn from
// the middle of the disposer's emission; the specialized body's own `return`
// then built the destructor-body exc-restore `select` in the wrong function.
// ===========================================================================
BEZEL_TEST(destructor_body_nested_fn_2872, self_method_call_generic_owner_spec_28_4) {
    BEZEL_EXPECT_EQ(runFull(
        "module Main; "
        "public class Box<T> "
        "  public v: T; "
        "  public tag: int; "
        "  public static Trace: int; "
        "  public method Helper(n: int): int return n * 2; end Helper; "
        "  destructor "
        "    var k := 5; "
        "    Trace := 100 + Helper(tag) + k; "
        "  end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  do using var b := new Box<string>(); b.tag := 6; end; "
        "  return Box<string>.Trace; "
        "end Main; end C;"), 117);
}

// ACCEPT-CONTROL, not a defect observer: with a NON-generic owner the self-call
// needs no specialization, so no second function is emitted and this spelling
// compiled before the fix too. It is here to pin that the fix does not narrow
// the ordinary case. (Verified: it is one of the three that pass with the fix
// patched out.)
BEZEL_TEST(destructor_body_nested_fn_2872, self_method_call_nongeneric_owner_spec_28_4) {
    BEZEL_EXPECT_EQ(runFull(
        "module Main; "
        "public class Box "
        "  public tag: int; "
        "  public static Trace: int; "
        "  public method Helper(n: int): int return n * 2; end Helper; "
        "  destructor "
        "    var k := 5; "
        "    Trace := 100 + Helper(tag) + k; "
        "  end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  do using var b := new Box(); b.tag := 6; end; "
        "  return Box.Trace; "
        "end Main; end C;"), 117);
}

// ===========================================================================
// Shape 2 under memory checking, loop-amplified. A constructor writing / a
// disposal running at the wrong place shows up as a live-object imbalance, not
// only as a crash, so count creations against disposals over many iterations
// and let LSan see the whole run. (Shape 1 is deliberately NOT run under
// runAotChecked: a lambda's closure cell allocated in a destructor body leaks
// independently of this fix — the SINGLE-lambda spelling, which compiled fine
// before it, leaks identically. Filed separately.)
// ===========================================================================
BEZEL_TEST(destructor_body_nested_fn_2872, self_method_call_loop_amplified_disposes_once) {
    auto r = runAotChecked(
        "module Main; "
        "public class Box<T> "
        "  public v: T; "
        "  public tag: int; "
        "  public static Made: int; "
        "  public static Gone: int; "
        "  public constructor() Box<T>.Made := Box<T>.Made + 1; end constructor; "
        "  public method Helper(n: int): int return n * 2; end Helper; "
        "  destructor "
        "    var k := 1; "
        "    Box<T>.Gone := Box<T>.Gone + Helper(tag) + k; "
        "  end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  for var i in 1..15 do "
        "    using var b := new Box<string>(); "
        "    b.tag := 1; "
        "  end; "
        // 15 constructions; each destructor adds Helper(1)+1 = 3.
        "  return Box<string>.Made * 10 + Box<string>.Gone; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 195);   // 15*10 + 45
}

// ===========================================================================
// CONTROL — #1468/#1607 must still hold. A destructor that runs WHILE AN
// EXCEPTION IS PROPAGATING and touches a user static (which emits the §22.13
// static-init guard's pending-check, the very check the entry-exc value
// adjusts) must not abandon the rest of the disposal. §28.4: "Before it
// propagates, the remaining steps of the generated `Dispose()` still run: the
// remaining owned-field disposals (reverse declaration order) and the
// base-class chain complete". The fix narrows that adjustment to the disposer's
// OWN function, so this in-function case is untouched: `touched` and the owned
// field's `disposed` are both 1, and the run is leak-clean.
// ===========================================================================
BEZEL_TEST(destructor_body_nested_fn_2872, unwind_disposal_static_touch_still_completes) {
    auto r = runAotChecked(
        "module Main; "
        "class Inner "
        "  public static disposed: int := 0; "
        "  destructor Inner.disposed := Inner.disposed + 1; end destructor; "
        "end Inner; "
        "class Outer "
        "  public static touched: int := 0; "
        "  private owned _inner: Inner; "
        "  public constructor() _inner := new Inner(); end constructor; "
        "  destructor Outer.touched := Outer.touched + 1; end destructor; "
        "end Outer; "
        "class C public static method Main(): int "
        "  var n := 0; "
        "  try "
        "    using var o := new Outer(); "
        "    throw new InvalidOperationException(\"boom\"); "
        "  catch e: InvalidOperationException => "
        "    n := 100 + Outer.touched * 10 + Inner.disposed; "
        "  end; "
        "  return n; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 111);
}

// CONTROL — the same unwind shape with a destructor body that ALSO calls one of
// its own methods, i.e. both halves at once: the nested emission happens, AND
// an exception is in flight through the disposal. The self-call's result must
// land and the owned field must still be disposed exactly once.
BEZEL_TEST(destructor_body_nested_fn_2872, unwind_disposal_with_self_call_completes) {
    auto r = runAotChecked(
        "module Main; "
        "class Inner "
        "  public static disposed: int := 0; "
        "  destructor Inner.disposed := Inner.disposed + 1; end destructor; "
        "end Inner; "
        "class Outer "
        "  public static touched: int := 0; "
        "  private owned _inner: Inner; "
        "  public constructor() _inner := new Inner(); end constructor; "
        "  public method Bump(n: int): int return n + 1; end Bump; "
        "  destructor Outer.touched := Bump(Outer.touched); end destructor; "
        "end Outer; "
        "class C public static method Main(): int "
        "  var n := 0; "
        "  try "
        "    using var o := new Outer(); "
        "    throw new InvalidOperationException(\"boom\"); "
        "  catch e: InvalidOperationException => "
        "    n := 100 + Outer.touched * 10 + Inner.disposed; "
        "  end; "
        "  return n; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 111);
}
