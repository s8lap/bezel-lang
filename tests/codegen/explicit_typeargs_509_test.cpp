// Codegen — explicit method-level type arguments at the call site (issue #509).
//
// §14.1 / §14.2: generic methods are monomorphized at each call site; the
// return type is the concrete substituted type.  The compiler normally infers
// the method-level type parameter T from the call arguments (fixed by #504) or
// from the return-target type (fixed by #391).  When the programmer supplies
// the type argument explicitly — `Wrapper.Identity<int>(42)` — the parser
// attaches those explicit types to the MemberAccessExpr as `typeArgs`, and
// sema's `typeCall` must use them to substitute T in the return type.
//
// Before this fix:
//   * The return type stayed as the bare unresolved `T` TypeRef (no substitution
//     was performed when typeArgs were present on the callee).
//   * For a primitive return like int: the call expr typed as T (i32 in codegen
//     but matched as the wrong type → comparison with a different LLVM type →
//     LLVM ICmpInst::AssertOK SIGABRT or silent wrong result).
//   * For a string return: the call expr typed as T instead of string → the
//     string-equality comparison tried to compare an i32 against a string ptr →
//     IR mistype → SIGABRT.
//   * For a class return: the call expr typed as T → codegen lowered the return
//     as i32 (generic placeholder) → incorrect value or crash.
//   * For a constrained generic (`where T: class`) called with explicit class
//     type arg: the return type was i32 (bare T) → IR type mismatch → SIGABRT.
//
// After this fix: explicit typeArgs on a MemberAccessExpr are looked up in
// sema and used to substitute the return type, exactly as the inferred path
// does in #504.
//
// Spec references:
//   §14.1 — generic methods: the return type is the substituted type, not the
//            bare type parameter, at each call site.
//   §14.2 — constraint-bound substitution: `where T: class` type args supplied
//            explicitly must also produce the correct concrete return type.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

// Helper: compile+JIT and return exit code; -999 on compile/JIT failure.
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}

// Helper: compile+JIT and return the full JitResult (for checking diagnostics).
bezel::test::JitResult jit(const std::string& prog) {
    return runJit(prog);
}

// Shared wrapper class used by several tests.
const char* kWrapper =
    "module M; "
    "class Wrapper "
    "  public static method Identity<T>(v: T): T "
    "    return v; "
    "  end Identity; "
    "end Wrapper; ";

const char* kW =
    "module M; "
    "class W "
    "  public static method Id<T>(v: T): T "
    "    return v; "
    "  end Id; "
    "end W; ";

}  // namespace

// ── Primary cases: explicit int type arg ─────────────────────────────────────

// §14.1 / #509: `Identity<int>(42)` with explicit T=int returns 42, not 0.
// Before fix: return type stays bare T; codegen emits i32 placeholder; the
// result of the call is incorrectly typed → comparison with literal 42 is
// typed wrong → returns 0 (or crashes).
BEZEL_TEST(codegen_explicit_typeargs_509, identity_explicit_int_returns_42_14_1) {
    BEZEL_EXPECT_EQ(run(
        std::string(kWrapper) +
        "class C "
        "  public static method Main(): int "
        "    var x := Wrapper.Identity<int>(42); "
        "    if x = 42 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// §14.1 / #509: explicit int, assigned directly to a typed local.
BEZEL_TEST(codegen_explicit_typeargs_509, identity_explicit_int_typed_local_14_1) {
    BEZEL_EXPECT_EQ(run(
        std::string(kWrapper) +
        "class C "
        "  public static method Main(): int "
        "    var x: int := Wrapper.Identity<int>(99); "
        "    return x; "
        "  end Main; "
        "end C;"), 99);
}

// §14.1 / #509: explicit int, return directly from method.
BEZEL_TEST(codegen_explicit_typeargs_509, identity_explicit_int_return_14_1) {
    BEZEL_EXPECT_EQ(run(
        std::string(kWrapper) +
        "class C "
        "  public static method Main(): int "
        "    return Wrapper.Identity<int>(77); "
        "  end Main; "
        "end C;"), 77);
}

// ── Explicit string type arg ──────────────────────────────────────────────────

// §14.1 / #509: `Id<string>("hi")` with explicit T=string returns "hi".
// Before fix: compiler SIGABRT (LLVM ICmpInst::AssertOK) because return type
// was i32 (bare T placeholder) instead of string ptr, and comparing with
// string literal produced a mismatched ICmp instruction.
BEZEL_TEST(codegen_explicit_typeargs_509, id_explicit_string_no_crash_14_1) {
    auto r = jit(
        std::string(kW) +
        "class C "
        "  public static method Main(): int "
        "    var s := W.Id<string>(\"hi\"); "
        "    if s = \"hi\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// §14.1 / #509: explicit string, round-trip through Identity.
BEZEL_TEST(codegen_explicit_typeargs_509, identity_explicit_string_round_trip_14_1) {
    BEZEL_EXPECT_EQ(run(
        std::string(kWrapper) +
        "class C "
        "  public static method Main(): int "
        "    var s := Wrapper.Identity<string>(\"hello\"); "
        "    if s = \"hello\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// ── Explicit class type arg ───────────────────────────────────────────────────

// §14.1 / #509: explicit class type arg: `Identity<A>(a)` where A is a class.
// Return type must be A (ptr), not i32 (bare T placeholder). The class has a
// field v=42; reading v after round-trip must return 42.
BEZEL_TEST(codegen_explicit_typeargs_509, identity_explicit_class_arg_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class A "
        "  public v: int; "
        "  public constructor() v := 42; end constructor; "
        "  destructor end destructor; "
        "end A; "
        "class Wrapper "
        "  public static method Identity<T>(x: T): T "
        "    return x; "
        "  end Identity; "
        "end Wrapper; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new A(); "
        "    var a2 := Wrapper.Identity<A>(a); "
        "    return a2.v; "
        "  end Main; "
        "end C;"), 42);
}

// ── Constrained generic with explicit class type arg ─────────────────────────

// §14.2 / #509: constrained generic `where T: class` called with explicit
// class type arg. Before fix: result typed as i1 (bool) not i32 due to
// relational-op misparse producing a bool that codegen tried to return as int.
BEZEL_TEST(codegen_explicit_typeargs_509, constrained_explicit_class_arg_no_irfail_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box "
        "  public n: int; "
        "  public constructor(v: int) n := v; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "static class Util "
        "  public static method Pass<T>(x: T): T where T: class "
        "    return x; "
        "  end Pass; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(7); "
        "    var b2 := Util.Pass<Box>(b); "
        "    return b2.n; "
        "  end Main; "
        "end C;"), 7);
}

// ── Return from a typed method ────────────────────────────────────────────────

// §14.1 / #509: explicit type arg in a return statement from a typed method
// must not cause a false "return type mismatch" error.
BEZEL_TEST(codegen_explicit_typeargs_509, explicit_typearg_in_return_no_false_error_14_1) {
    auto r = jit(
        std::string(kWrapper) +
        "class C "
        "  public static method GetInt(): int "
        "    return Wrapper.Identity<int>(55); "
        "  end GetInt; "
        "  public static method Main(): int "
        "    return C.GetInt(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);
}

// ── Control: inferred type args still work (#504 regression guard) ────────────

// §14.1 / #504 regression: inferred T=int via argument — must still work after
// the #509 fix (the explicit-typearg path must not break the inferred path).
BEZEL_TEST(codegen_explicit_typeargs_509, control_inferred_int_still_works_14_1) {
    BEZEL_EXPECT_EQ(run(
        std::string(kWrapper) +
        "class C "
        "  public static method Main(): int "
        "    return Wrapper.Identity(42); "
        "  end Main; "
        "end C;"), 42);
}

// §14.1 / #504 regression: inferred T=record — must still work.
BEZEL_TEST(codegen_explicit_typeargs_509, control_inferred_record_still_works_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record OneFld(x: int); "
        "class Wrapper "
        "  public static method Identity<T>(v: T): T "
        "    return v; "
        "  end Identity; "
        "end Wrapper; "
        "class C "
        "  public static method Main(): int "
        "    var r := new OneFld(88); "
        "    return Wrapper.Identity(r).x; "
        "  end Main; "
        "end C;"), 88);
}

// §14.2 / #484 regression: constraint-dispatch via inferred type arg.
BEZEL_TEST(codegen_explicit_typeargs_509, control_inferred_constrained_still_works_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IVal  method Val(): int; end IVal; "
        "class Num : IVal "
        "  public _n: int := 0; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public method Val(): int => _n; "
        "  destructor end destructor; "
        "end Num; "
        "static class Utils "
        "  public static method GenGet<T>(x: T): int where T: IVal "
        "    return x.Val(); "
        "  end GenGet; "
        "end Utils; "
        "class C "
        "  public static method Main(): int "
        "    using var n := new Num(33); "
        "    return Utils.GenGet(n); "
        "  end Main; "
        "end C;"), 33);
}

// ── Multi-arg generic: explicit type arg with non-T arg ──────────────────────

// §14.1 / #509: explicit type arg on a method with T in return and in one
// param but not all — `First<T>(a: T, b: int): T`.
BEZEL_TEST(codegen_explicit_typeargs_509, explicit_typearg_first_of_two_params_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Util "
        "  public static method First<T>(a: T, b: int): T "
        "    return a; "
        "  end First; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    return Util.First<int>(13, 0); "
        "  end Main; "
        "end C;"), 13);
}
