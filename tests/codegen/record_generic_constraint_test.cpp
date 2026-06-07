// Codegen — a *record* (value type) implementing an interface, passed as a
// generic constraint argument (§14 generics, §14.2 constraints).
//
// §14.2: "A record conforming to an interface may be passed as the argument to
// a type parameter `T` constrained by `where T: IInterface`."  When such a
// generic method is monomorphized with a *value*-type argument (record/struct
// or a primitive), the substituted parameter type is a by-value struct, not a
// pointer.
//
// The miscompile: `Calc.GetVal<T>(x: T): int where T: INum` monomorphized with
// T=Point (a record) emitted the specialized function with the generic
// placeholder parameter type (i32) instead of the substituted value type's
// LLVM struct (`%record.Point`). The call site passed the record struct by
// value, producing an IR verifier failure:
//   Call parameter type does not match function signature!
//   %record.Point %8 = call i32 @"bz$Calc$GetVal"(%record.Point %6)
// — the declaration's parameter was typed i32 but the call passed a struct.
//
// Class type arguments already worked (a class = a pointer, so the placeholder
// happened to match). The fix makes a value-type substitution yield the
// substituted type's real LLVM struct for the parameter (and return) type, and
// makes `x.Val()` inside the concrete-T body dispatch directly to the record's
// implementation (the receiver is a record value, not a pointer).
//
// Each test is load-bearing: the record cases crash the IR verifier before the
// fix; the class control still returns the correct value; the non-generic
// direct-call control verifies record→interface dispatch is sound on its own.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── (a) the repro: record T constrained `where T: INum` → returns 7 ──────────

// §14.2: `Point` is a record implementing `INum`; passed as T to
// `GetVal<T>(x: T): int where T: INum`, the body `x.Val()` returns X+Y = 7.
// Before the fix: IR verifier crash (param typed i32, call passes %record.Point),
// runJit().ran == false → -999.
BEZEL_TEST(record_generic_constraint, record_t_interface_constraint_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface INum  method Val(): int; end INum; "
        "record Point(X: int, Y: int) : INum "
        "  public method Val(): int => X + Y; "
        "end Point; "
        "static class Calc "
        "  public static method GetVal<T>(x: T): int where T: INum => x.Val(); "
        "end Calc; "
        "class C public static method Main(): int "
        "  var p := new Point(3, 4); "
        "  return Calc.GetVal(p); "      // 3 + 4 = 7
        "end Main; end C;"), 7);
}

// §14.2: same, with a statement-body generic method (a distinct lowering path
// from the expression-body form above).
BEZEL_TEST(record_generic_constraint, record_t_stmt_body_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface INum  method Val(): int; end INum; "
        "record Point(X: int, Y: int) : INum "
        "  public method Val(): int => X + Y; "
        "end Point; "
        "static class Calc "
        "  public static method GetVal<T>(x: T): int where T: INum "
        "    return x.Val(); "
        "  end GetVal; "
        "end Calc; "
        "class C public static method Main(): int "
        "  var p := new Point(10, 11); "
        "  return Calc.GetVal(p); "      // 10 + 11 = 21
        "end Main; end C;"), 21);
}

// ── (b) a record T with multiple interface methods ───────────────────────────

// §14.2: the constraint interface declares two methods; both must dispatch to
// the record's implementations when called inside the monomorphized body.
BEZEL_TEST(record_generic_constraint, record_t_multiple_interface_methods_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IPair  method First(): int; method Second(): int; end IPair; "
        "record Pair(A: int, B: int) : IPair "
        "  public method First(): int => A; "
        "  public method Second(): int => B; "
        "end Pair; "
        "static class Calc "
        "  public static method Combine<T>(x: T): int where T: IPair "
        "    return x.First() * 10 + x.Second(); "
        "  end Combine; "
        "end Calc; "
        "class C public static method Main(): int "
        "  var p := new Pair(4, 2); "
        "  return Calc.Combine(p); "     // 4*10 + 2 = 42
        "end Main; end C;"), 42);
}

// ── (c) a primitive value-type T (int) in a generic method ───────────────────

// §14 / §14.2: an unconstrained generic method instantiated with a primitive
// value type (int). The parameter substitutes to the primitive's LLVM type and
// the body adds two of them. (No constraint member call — primitives have no
// user-declared interface members here; this exercises value-type substitution
// of a non-record value type through the same monomorphization path.)
BEZEL_TEST(record_generic_constraint, primitive_value_t_param_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "static class Calc "
        "  public static method Twice<T>(x: T): T where T: value => x; "
        "end Calc; "
        "class C public static method Main(): int "
        "  var n: int := 5; "
        "  var m: int := Calc.Twice(n); "
        "  return m + 16; "             // 5 + 16 = 21
        "end Main; end C;"), 21);
}

// ── (d) a generic method returning T where T = record ────────────────────────

// §14: a generic method whose *return* type is the type parameter, instantiated
// with a record. The return must be a by-value record struct, not a pointer or
// the i32 placeholder.
BEZEL_TEST(record_generic_constraint, generic_returns_record_t_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Point(X: int, Y: int); "
        "static class Calc "
        "  public static method Echo<T>(x: T): T where T: value => x; "
        "end Calc; "
        "class C public static method Main(): int "
        "  var p := new Point(8, 13); "
        "  var q: Point := Calc.Echo(p); "
        "  return q.X + q.Y; "          // 8 + 13 = 21
        "end Main; end C;"), 21);
}

// §14.2: a generic returning a record T, then calling the constraint member on
// the result — combines value-type return substitution with record→interface
// dispatch.
BEZEL_TEST(record_generic_constraint, generic_returns_record_then_dispatch_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface INum  method Val(): int; end INum; "
        "record Point(X: int, Y: int) : INum "
        "  public method Val(): int => X + Y; "
        "end Point; "
        "static class Calc "
        "  public static method Echo<T>(x: T): T where T: INum => x; "
        "end Calc; "
        "class C public static method Main(): int "
        "  var p := new Point(9, 12); "
        "  var q: Point := Calc.Echo(p); "
        "  return q.Val(); "           // 9 + 12 = 21
        "end Main; end C;"), 21);
}

// ── (e) CONTROL: class T constraint still works ──────────────────────────────

// §14.2: a *class* type argument must keep returning the correct value (the
// case that already worked — a regression guard for the fix).
BEZEL_TEST(record_generic_constraint, class_t_constraint_control_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface INum  method Val(): int; end INum; "
        "class Num : INum "
        "  public _n: int := 0; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public method Val(): int => _n; "
        "  destructor end destructor; "
        "end Num; "
        "static class Calc "
        "  public static method GetVal<T>(x: T): int where T: INum => x.Val(); "
        "end Calc; "
        "class C public static method Main(): int "
        "  using var n := new Num(7); "
        "  return Calc.GetVal(n); "     // 7
        "end Main; end C;"), 7);
}

// ── (f) CONTROL: the non-generic direct call `point.Val()` works ─────────────

// §14.2 / §12: a record implementing an interface, with its interface method
// called directly (non-generic). This must already work; it is the reference
// for how the monomorphized body should dispatch.
BEZEL_TEST(record_generic_constraint, non_generic_record_direct_call_control_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface INum  method Val(): int; end INum; "
        "record Point(X: int, Y: int) : INum "
        "  public method Val(): int => X + Y; "
        "end Point; "
        "class C public static method Main(): int "
        "  var p := new Point(3, 4); "
        "  return p.Val(); "           // 7
        "end Main; end C;"), 7);
}
