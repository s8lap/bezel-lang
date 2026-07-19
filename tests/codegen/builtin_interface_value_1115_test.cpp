// Spec-derived tests for #1115 (path-2): the auto-imported builtin generic
// interfaces IComparable<T> (stdlib §2.8) and IEquatable<T> (§2.9) are usable as
// ordinary interface-typed VALUES — fields, parameters, returns, locals, heap
// references — dispatching through the itable like a user-declared interface.
//
// Before path-2 these were declaration-less (registered for name resolution but
// with no synthesized InterfaceDecl/vtable), so used as a value type they
// IR-failed ("Call parameter type does not match"). The fix synthesizes a real
// Interface TypeSymbol for each (carrying the CompareTo/Equals contract member),
// so every codegen layer — which keys on declaration->kind==Interface — picks
// them up with no new codegen. Records implementing IComparable (#1141) and the
// `where T: IComparable<T>` constraint form keep working unchanged.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
const char* CMP =
    "module M; "
    "class C: IComparable<int> "
    "  private _v: int; "
    "  public constructor(v: int) _v := v; end constructor; "
    "  public method CompareTo(other: int): int => (2 if _v > other else 0); "
    "end C; ";
const char* EQ =
    "module M; "
    "class C: IEquatable<int> "
    "  private _v: int; "
    "  public constructor(v: int) _v := v; end constructor; "
    "  public method Equals(other: int): bool => _v = other; "
    "end C; ";
// #975 / stdlib §5: a user class implementing IEqualityComparer<int> — the
// "agent" interface with TWO members (Equals(a,b):bool, GetHashCode(value):int),
// the first taking TWO generic-`T` parameters. Same itable dispatch path as
// IComparable/IEquatable; exercises the multi-method / multi-param synthesis.
const char* EQC =
    "module M; "
    "class Cmp: IEqualityComparer<int> "
    "  public method Equals(a: int, b: int): bool => a = b; "
    "  public method GetHashCode(value: int): int => value * 2; "
    "end Cmp; ";
}  // namespace

// #1115: IComparable<int> as a PARAMETER type — dispatch through the interface.
BEZEL_TEST(builtin_interface_value_1115, icomparable_as_param_dispatches) {
    BEZEL_EXPECT_EQ(run(
        std::string(CMP) +
        "class App "
        "  private static method Use(x: IComparable<int>): int => x.CompareTo(3); "
        "  public static method Main(): int "
        "    using var c := new C(5); return Use(c); "          // 5>3 → 2
        "  end Main; end App;"), 2);
}

// #1115: IComparable<int> as a LOCAL variable type — dispatch.
BEZEL_TEST(builtin_interface_value_1115, icomparable_as_local_dispatches) {
    BEZEL_EXPECT_EQ(run(
        std::string(CMP) +
        "class App public static method Main(): int "
        "  using var c := new C(5); var x: IComparable<int> := c; "
        "  return x.CompareTo(3); "                             // 2
        "end Main; end App;"), 2);
}

// #1115: IEquatable<int> as a value + Equals dispatch.
BEZEL_TEST(builtin_interface_value_1115, iequatable_as_value_dispatches) {
    BEZEL_EXPECT_EQ(run(
        std::string(EQ) +
        "class App public static method Main(): int "
        "  using var c := new C(5); var e: IEquatable<int> := c; "
        "  return (7 if e.Equals(5) else 0) + (1 if e.Equals(9) else 0); "  // 7
        "end Main; end App;"), 7);
}

// #1115: IComparable<int> as a RETURN type. v0.201 (#2048): the fresh-producing
// factory is spelled `owned IComparable<int>` per the declaration-site static-
// return rule.
BEZEL_TEST(builtin_interface_value_1115, icomparable_as_return_type) {
    BEZEL_EXPECT_EQ(run(
        std::string(CMP) +
        "class App "
        "  private static method Make(): owned IComparable<int> => new C(9); "
        "  public static method Main(): int return Make().CompareTo(3); end Main; "  // 2
        "end App;"), 2);
}

// #1115: IComparable<int> as a FIELD type, read then dispatched.
BEZEL_TEST(builtin_interface_value_1115, icomparable_as_field_type) {
    BEZEL_EXPECT_EQ(run(
        std::string(CMP) +
        "class Box "
        "  public owned cmp: IComparable<int>; "
        "  public constructor() cmp := new C(9); end constructor; "
        "  public static method Main(): int "
        "    using var b := new Box(); return b.cmp.CompareTo(3); "             // 2
        "  end Main; end Box;"), 2);
}

// #1115 keep-green: the `where T: IComparable<T>` constraint form is unchanged.
BEZEL_TEST(builtin_interface_value_1115, where_icomparable_constraint_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System; "
        "record Money(cents: int): IComparable<Money> "
        "  public method CompareTo(other: Money): int => "
        "    (1 if cents > other.cents else (-1 if cents < other.cents else 0)); "
        "end Money; "
        "class App public static method Main(): int "
        "  var a := new Money(150); var b := new Money(99); "
        "  return Math.Min<Money>(a, b).cents; "                // 99 (#1141 path)
        "end Main; end App;"), 99);
}

// #975: IEqualityComparer<int> as a PARAMETER type — BOTH members dispatch
// through the interface (Equals's two `T` params + GetHashCode's single one).
BEZEL_TEST(builtin_interface_value_1115, iequalitycomparer_as_param_dispatches) {
    BEZEL_EXPECT_EQ(run(
        std::string(EQC) +
        "class App "
        "  private static method Use(c: IEqualityComparer<int>): int => "
        "    (10 if c.Equals(5, 5) else 0) + c.GetHashCode(7); "   // 10 + 14
        "  public static method Main(): int "
        "    using var cmp := new Cmp(); return Use(cmp); "        // 24
        "  end Main; end App;"), 24);
}

// #975: IEqualityComparer<int> as a LOCAL — Equals false branch + GetHashCode.
BEZEL_TEST(builtin_interface_value_1115, iequalitycomparer_as_local_dispatches) {
    BEZEL_EXPECT_EQ(run(
        std::string(EQC) +
        "class App public static method Main(): int "
        "  using var cmp := new Cmp(); var c: IEqualityComparer<int> := cmp; "
        "  return (3 if c.Equals(4, 4) else 0) "                   // 3
        "       + (9 if c.Equals(4, 8) else 0) "                   // + 0 (false)
        "       + c.GetHashCode(6); "                              // + 12 -> 15
        "end Main; end App;"), 15);
}

// #975: IEqualityComparer<int> as a RETURN type — dispatch on the returned ref.
// v0.201 (#2048): the fresh-producing factory is spelled `owned IEqualityComparer<int>`
// per the declaration-site static-return rule.
BEZEL_TEST(builtin_interface_value_1115, iequalitycomparer_as_return_type) {
    BEZEL_EXPECT_EQ(run(
        std::string(EQC) +
        "class App "
        "  private static method Make(): owned IEqualityComparer<int> => new Cmp(); "
        "  public static method Main(): int "
        "    using var c := Make(); return c.GetHashCode(11); "    // 22
        "  end Main; end App;"), 22);
}

// #975: IEqualityComparer<int> as a FIELD type, read then dispatched.
BEZEL_TEST(builtin_interface_value_1115, iequalitycomparer_as_field_type) {
    BEZEL_EXPECT_EQ(run(
        std::string(EQC) +
        "class Box "
        "  public owned cmp: IEqualityComparer<int>; "
        "  public constructor() cmp := new Cmp(); end constructor; "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    return (8 if b.cmp.Equals(2, 2) else 0) + b.cmp.GetHashCode(3); " // 8 + 6
        "  end Main; end Box;"), 14);
}

// ── #1183 / §11.2 / §12.1: builtin generic-interface itable thunks must use
// the substituted T's real ABI, not the i32 placeholder. Previously when T is
// not int (e.g. a record struct, string ptr, or wide primitive), the itable
// thunk built by ensureNullableWideningThunk used ifaceTy (i32 placeholder)
// as the external signature and tried to forward that i32 to the concrete-T
// parameter — producing "Call parameter type does not match" IR-verify failure.
// The fix: (1) ensureNullableWideningThunk returns nullptr for non-widening
// mismatches so the itable slot holds the concrete function directly; (2) the
// call site rebuilds fnTy from the actual arg types when a mismatch is found.

// #1183: IComparable<Pt> where Pt is a record ({i32,i32} — struct T).
BEZEL_TEST(builtin_interface_value_1183, icomparable_record_t_dispatches) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(x: int, y: int); "
        "class PtC: IComparable<Pt> "
        "  public method CompareTo(other: Pt): int => other.x; "
        "end PtC; "
        "class App public static method Main(): int "
        "  using var p := new PtC(); var c: IComparable<Pt> := p; "
        "  return c.CompareTo(new Pt(42, 0)); "  // 42
        "end Main; end App;"), 42);
}

// #1183: IEquatable<string> where string lowers to ptr.
BEZEL_TEST(builtin_interface_value_1183, iequatable_string_t_dispatches) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class SC: IEquatable<string> "
        "  public method Equals(other: string): bool => other = \"hello\"; "
        "end SC; "
        "class App public static method Main(): int "
        "  using var s := new SC(); var e: IEquatable<string> := s; "
        "  return (7 if e.Equals(\"hello\") else 0) "  // 7
        "       + (3 if e.Equals(\"world\") else 0); "  // +0 → 7
        "end Main; end App;"), 7);
}

// #1183: IComparable<long> where long lowers to i64 (wide primitive T).
BEZEL_TEST(builtin_interface_value_1183, icomparable_long_t_dispatches) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class LC: IComparable<long> "
        "  private _v: long; "
        "  public constructor(v: long) _v := v; end constructor; "
        "  public method CompareTo(other: long): int => "
        "    (5 if _v > other else (-1 if _v < other else 0)); "
        "end LC; "
        "class App public static method Main(): int "
        "  using var c := new LC(100L); var x: IComparable<long> := c; "
        "  return x.CompareTo(50L) + x.CompareTo(100L); "  // 5 + 0 = 5
        "end Main; end App;"), 5);
}

// #1183: IEqualityComparer<Pt> where both T params are record structs.
BEZEL_TEST(builtin_interface_value_1183, iequalitycomparer_record_t_dispatches) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(x: int, y: int); "
        "class PtCmp: IEqualityComparer<Pt> "
        "  public method Equals(a: Pt, b: Pt): bool => a.x = b.x; "
        "  public method GetHashCode(value: Pt): int => value.x; "
        "end PtCmp; "
        "class App public static method Main(): int "
        "  using var cmp := new PtCmp(); var c: IEqualityComparer<Pt> := cmp; "
        "  return (10 if c.Equals(new Pt(3, 0), new Pt(3, 9)) else 0) "  // 10
        "       + c.GetHashCode(new Pt(7, 0)); "                          // + 7 = 17
        "end Main; end App;"), 17);
}

// #1183: IEqualityComparer<string> where both T params are string (ptr).
BEZEL_TEST(builtin_interface_value_1183, iequalitycomparer_string_t_dispatches) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class SCmp: IEqualityComparer<string> "
        "  public method Equals(a: string, b: string): bool => a = b; "
        "  public method GetHashCode(value: string): int => 42; "
        "end SCmp; "
        "class App public static method Main(): int "
        "  using var cmp := new SCmp(); var c: IEqualityComparer<string> := cmp; "
        "  return (10 if c.Equals(\"x\", \"x\") else 0) "  // 10
        "       + c.GetHashCode(\"z\"); "                   // + 42 = 52
        "end Main; end App;"), 52);
}

// #1183 keep-green: IComparable<int> (the existing T=i32 case) is unchanged.
BEZEL_TEST(builtin_interface_value_1183, icomparable_int_unchanged) {
    BEZEL_EXPECT_EQ(run(
        std::string(CMP) +
        "class App public static method Main(): int "
        "  using var c := new C(5); var x: IComparable<int> := c; "
        "  return x.CompareTo(3); "  // 5 > 3 → 2
        "end Main; end App;"), 2);
}
