// Codegen — virtual generic method with nullable value-type formal: S? -> T? lift
// not applied at emitVirtualGenericDispatch (issue #5090).
//
// #5090 sibling of #5046: the non-virtual paths (emitInlineGenericMethod,
// emitGenericSpecialization) call wrapArgForNullableValueFormal to lift bare
// arguments to {i8, T} when the formal is a nullable value type T?.  The virtual
// dispatch path (emitVirtualGenericDispatch -> specForImpl -> buildSpecializedMethodFn)
// was missing this wrap: args arrived as bare i32 / double but the specialized
// function's parameter was declared as {i8, i32} / {i8, double}, and the body
// returned {i8, i32}/{i8, double} against a {i8, double} return type.
//
// Repro (same GBase<T> shape as #2896's regression tests — instanceLayoutDiffersFromErasedTemplate
// is true so dispatch routes through emitVirtualGenericDispatch):
//   GBase<double>.Id(x: T?) — T? = double? at this instantiation.
//   Call: s.Id(q) where q: int? = 5
//   Error: ret { i8, i32 } against { i8, double } function return type
//
// Root cause: specForImpl built paramTys from raw args without calling
// wrapArgForNullableValueFormal, then buildSpecializedMethodFn declared the
// function with the bare-argument types.  The specialized body returned the
// nullable struct ({i8, i32}) against a {i8, double} return type → IR verify fail.
//
// Fix: after setting up the per-impl substitution, walk args and wrap each for its
// nullable value formal, then rebuild paramTys from the wrapped args — matching
// what emitGenericSpecialization does.
//
// Spec references:
//   §5.1 — nullable conversions and S? -> T? lift
//   §14.1 — generic method monomorphization
//   §12   — virtual dispatch on generic methods
//   v0.307

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// GBase<T> with a virtual Id<T>(x: T?): T? — the T? formal requires nullable
// lift at the call site.  instanceLayoutDiffersFromErasedTemplate is true for
// GBase<double>, so the call routes through emitVirtualGenericDispatch.
const char* kGBase =
    "module H; "
    "class Cell public tag: int := 3; end Cell; "
    "class GBase<T> "
    "  public v: T; "
    "  public owned cell: Cell := new Cell(); "
    "  public a: int := 41; "
    "  public virtual method Id(x: T?): T? return x; end Id; "
    "end GBase; ";

}  // namespace

// ── #5090 / §5.1 / §14.1 / §12: virtual generic T? formal — direct call ─────────

// The primary repro from the issue: GBase<double>.Id(int?) where T?=double?.
// Before fix: IR verification failure "Function return type does not match operand
// type of return inst!  ret { i8, i32 } %3  { i8, double }".
// After fix: compiles successfully and returns 5.
BEZEL_TEST(virtual_generic_nullable_formal_5090, direct_call_double_formal_int_arg) {
    // Must compile without IR verification error and return 5.
    // Uses int(v) to convert double to int for the exit code (avoids float comparison).
    BEZEL_EXPECT_EQ(run(
        std::string(kGBase) +
        "class C "
        "  public static method Main(): int "
        "    using var s := new GBase<double>(); "
        "    var q: int? := 5; "
        "    var r: double? := s.Id(q); "
        "    if r = null then return 0; end; "
        "    var v: double := r!; "
        "    return int(v); "
        "  end Main; "
        "end C;"), 5);
}

// Same shape, null value — the nullable lift must preserve the null.
BEZEL_TEST(virtual_generic_nullable_formal_5090, direct_call_double_formal_null) {
    BEZEL_EXPECT_EQ(run(
        std::string(kGBase) +
        "class C "
        "  public static method Main(): int "
        "    using var s := new GBase<double>(); "
        "    var q: int?; "
        "    var r: double? := s.Id(q); "
        "    if r is null then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// Same T?=double?, argument is bare double (implicit int->double conversion then lift).
BEZEL_TEST(virtual_generic_nullable_formal_5090, direct_call_double_formal_bare_double) {
    BEZEL_EXPECT_EQ(run(
        std::string(kGBase) +
        "class C "
        "  public static method Main(): int "
        "    using var s := new GBase<double>(); "
        "    var r: double? := s.Id(3.14); "
        "    if r = null then return 0; end; "
        "    var v: double := r!; "
        "    return int(v); "
        "  end Main; "
        "end C;"), 3);
}

// ── #5090 / §5.1 / §14.1 / §12: dispatch through base reference ────────────────

// GBase<T> as base-typed reference holding GBase<double> — dynamic dispatch
// through emitVirtualGenericDispatch (the default branch case).
BEZEL_TEST(virtual_generic_nullable_formal_5090, base_recv_derived_dispatch) {
    BEZEL_EXPECT_EQ(run(
        std::string(kGBase) +
        "class App "
        "  public static method Main(): int "
        "    using var s := new GBase<double>(); "
        "    var b: GBase<double> := s; "
        "    var q: int? := 7; "
        "    var r: double? := b.Id(q); "
        "    if r = null then return 0; end; "
        "    var v: double := r!; "
        "    return int(v); "
        "  end Main; "
        "end App;"), 7);
}

// ── Regression guards ──────────────────────────────────────────────────────────

// A virtual generic method with NON-nullable T formal must still work.
BEZEL_TEST(virtual_generic_nullable_formal_5090, nonnullable_t_formal_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class GBase<T> "
        "  public virtual method Id(x: T): T return x; end Id; "
        "end GBase; "
        "class C "
        "  public static method Main(): int "
        "    using var s := new GBase<double>(); "
        "    if s.Id(3.14) = 3.14 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// Non-virtual generic method with T? formal — existing path (emitGenericSpecialization).
BEZEL_TEST(virtual_generic_nullable_formal_5090, nonvirtual_t_formal_regression) {
    BEZEL_EXPECT_EQ(run(
        std::string(kGBase) +
        "class Plain "
        "  public method Id<T>(x: T?): T? return x; end Id; "
        "end Plain; "
        "class C "
        "  public static method Main(): int "
        "    using var p := new Plain(); "
        "    var q: int? := 6; "
        "    var r: double? := p.Id<double>(q); "
        "    if r = null then return 0; end; "
        "    var v: double := r!; "
        "    return int(v); "
        "  end Main; "
        "end C;"), 6);
}
