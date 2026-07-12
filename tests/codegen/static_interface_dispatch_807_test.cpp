// Spec-derived codegen tests for §11.8 (v0.176, #807) — static interface
// member DISPATCH through a constrained type parameter, resolved at each
// generic instantiation (no vtables, no runtime dispatch):
//
//   public static method Sum<T>(items: Array<T>): T where T: INumber<T>
//     var total := T.Zero();
//     for var x in items do total := total + x; end;
//     return total;
//   end Sum;                                       (the spec's own example)
//
// For PRIMITIVE T the compiler-provided §2.19 conformances bridge the
// intrinsic/declared split: INumber.Zero()/One() lower to the constants, the
// operators to the intrinsic ops (§3.5), ISteppable.Step to the integer/char
// advance, IParsable.Parse/TryParse to the existing runtime parse lowerings.
// For a USER type the call lowers to a direct call of the concrete type's
// static member. Authority: spec §11.8 (v0.176), stdlib §2.19.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "support/aot_support.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool rejects(const std::string& src) {
    auto r = runAot(src);
    return !r.ran && r.diagnostics.hasErrors();
}

// The spec §11.8 Sum example, verbatim shape.
const char* kSum =
    "static class F "
    "  public static method Sum<T>(items: Array<T>): T where T: INumber<T> "
    "    var total := T.Zero(); "
    "    for var x in items do total := total + x; end; "
    "    return total; "
    "  end Sum; "
    "end F; ";

// A user INumber<T>-conforming class (stdlib §2.19 + §25.2 complete groups).
const char* kMeters =
    "public class Meters: INumber<Meters> "
    "  public Value: int; "
    "  public constructor(v: int) this.Value := v; end; "
    "  public static method Zero(): Meters => new Meters(0); "
    "  public static method One(): Meters => new Meters(1); "
    "  public static method operator +(a: Meters, b: Meters): Meters => "
    "    new Meters(a.Value + b.Value); "
    "  public static method operator -(a: Meters, b: Meters): Meters => "
    "    new Meters(a.Value - b.Value); "
    "  public static method operator *(a: Meters, b: Meters): Meters => "
    "    new Meters(a.Value * b.Value); "
    "  public static method operator /(a: Meters, b: Meters): Meters => "
    "    new Meters(a.Value / b.Value); "
    "  public static method operator <(a: Meters, b: Meters): bool => "
    "    a.Value < b.Value; "
    "  public static method operator >(a: Meters, b: Meters): bool => "
    "    a.Value > b.Value; "
    "  public static method operator <=(a: Meters, b: Meters): bool => "
    "    a.Value <= b.Value; "
    "  public static method operator >=(a: Meters, b: Meters): bool => "
    "    a.Value >= b.Value; "
    "end Meters; ";

}  // namespace

// ── §11.8: the spec's Sum example over the primitive tower ──────────────────

BEZEL_TEST(static_iface_dispatch_807, sum_int_spec_example) {
    // per §11.8 (the section's own example), T = int
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kSum +
        "class C public static method Main(): int "
        "  using var xs := new Array<int>(); "
        "  xs.Add(4); xs.Add(5); xs.Add(6); "
        "  return F.Sum(xs); "
        "end Main; end C;"), 15);
}

BEZEL_TEST(static_iface_dispatch_807, sum_long) {
    // per §11.8 / §2.19, T = long
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kSum +
        "class C public static method Main(): int "
        "  using var xs := new Array<long>(); "
        "  xs.Add(100L); xs.Add(23L); "
        "  return int(F.Sum(xs)); "
        "end Main; end C;"), 123);
}

BEZEL_TEST(static_iface_dispatch_807, sum_double) {
    // per §11.8 / §2.19, T = double: 1.5 + 2.25 + 3.25 = 7.0
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kSum +
        "class C public static method Main(): int "
        "  using var xs := new Array<double>(); "
        "  xs.Add(1.5); xs.Add(2.25); xs.Add(3.25); "
        "  return int(F.Sum(xs) * 2.0); "
        "end Main; end C;"), 14);
}

// TWO instantiations of the same generic in one program stay independent.
BEZEL_TEST(static_iface_dispatch_807, sum_two_instantiations) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kSum +
        "class C public static method Main(): int "
        "  using var xs := new Array<int>(); xs.Add(30); xs.Add(7); "
        "  using var ys := new Array<long>(); ys.Add(5L); "
        "  return F.Sum(xs) + int(F.Sum(ys)); "
        "end Main; end C;"), 42);
}

// ── §11.8: a user type end-to-end (Meters: INumber<Meters>) ────────────────

BEZEL_TEST(static_iface_dispatch_807, sum_user_meters_value) {
    // per §11.8: T.Zero() and `total + x` lower to DIRECT calls of
    // Meters.Zero / Meters.operator+ at the T=Meters instantiation.
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kMeters + kSum +
        "class C public static method Main(): int "
        "  using var xs := new Array<Meters>(); "
        "  xs.Add(new Meters(10)); xs.Add(new Meters(13)); "
        "  xs.Add(new Meters(19)); "
        "  var t := F.Sum(xs); "
        "  return t.Value; "
        "end Main; end C;"), 42);
}

BEZEL_TEST(static_iface_dispatch_807, user_one_and_operators) {
    // T.One() plus the -,*,/ constraint operators on a user type.
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kMeters +
        "static class F "
        "  public static method Calc<T>(a: T, b: T): T where T: INumber<T> "
        "    var one := T.One(); "
        "    return (a - b) * (a + b) / (one + one); "
        "  end Calc; "
        "end F; "
        "class C public static method Main(): int "
        "  using var a := new Meters(7); "
        "  using var b := new Meters(3); "
        "  var r := F.Calc(a, b); "
        "  return r.Value; "
        "end Main; end C;"), 20);
}

// ── §11.8: constraint operators — comparison + unsigned semantics ──────────

BEZEL_TEST(static_iface_dispatch_807, min_via_constraint_lt_int) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F "
        "  public static method Min2<T>(a: T, b: T): T where T: INumber<T> "
        "    if a < b then return a; end; return b; "
        "  end Min2; "
        "end F; "
        "class C public static method Main(): int "
        "  return F.Min2(9, 4); "
        "end Main; end C;"), 4);
}

BEZEL_TEST(static_iface_dispatch_807, min_constraint_lt_unsigned_semantics) {
    // per §3.5/§6.2: T = uint must compare UNSIGNED — 4294967290u (a negative
    // value under a signed icmp) is NOT less than 5u.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F "
        "  public static method Min2<T>(a: T, b: T): T where T: INumber<T> "
        "    if a < b then return a; end; return b; "
        "  end Min2; "
        "end F; "
        "class C public static method Main(): int "
        "  var m := F.Min2(4294967290u, 5u); "
        "  if m = 5u then return 1; end; return 0; "
        "end Main; end C;"), 1);
}

BEZEL_TEST(static_iface_dispatch_807, min_user_type) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kMeters +
        "static class F "
        "  public static method Min2<T>(a: T, b: T): T where T: INumber<T> "
        "    if a < b then return a; end; return b; "
        "  end Min2; "
        "end F; "
        "class C public static method Main(): int "
        "  using var a := new Meters(9); "
        "  using var b := new Meters(4); "
        "  var m := F.Min2(a, b); "
        "  return m.Value; "
        "end Main; end C;"), 4);
}

// ── §25.2 complete-group entitlement (#1498): `>`,`<=`,`>=` on a `T`-typed
//    operand though the constraint (INumber<T>/ISteppable<T>) declares `<`
//    ALONE. Each use binds at instantiation to the conforming type's OWN
//    declaration of that operator; a primitive T uses its intrinsic set. The
//    repro `a > b` in a `Max2<Meters>` body previously emitted a primitive
//    `icmp` on the aggregate operands and aborted the LLVM verifier. ──────────

BEZEL_TEST(static_iface_dispatch_807, max_via_group_gt_user_type_1498) {
    // per §25.2 (v0.181): `a > b` routes to Meters.operator> even though
    // INumber<T> declares only `<`. The exact #1498 crash repro.
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kMeters +
        "static class F "
        "  public static method Max2<T>(a: T, b: T): T where T: INumber<T> "
        "    if a > b then return a; end; return b; "
        "  end Max2; "
        "end F; "
        "class C public static method Main(): int "
        "  using var a := new Meters(3); "
        "  using var b := new Meters(4); "
        "  var m := F.Max2(a, b); "
        "  return m.Value; "
        "end Main; end C;"), 4);
}

BEZEL_TEST(static_iface_dispatch_807, all_four_relations_user_type_1498) {
    // per §25.2: all of `<`,`>`,`<=`,`>=` route to Meters's own declarations
    // through the single `<`-declaring INumber<Meters> constraint. Encodes the
    // six expected bools (a=3,b=4,c=4) as a bitmask; every bit must be set → 63.
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kMeters +
        "static class F "
        "  public static method Lt<T>(a: T, b: T): bool where T: INumber<T> => a < b; "
        "  public static method Gt<T>(a: T, b: T): bool where T: INumber<T> => a > b; "
        "  public static method Le<T>(a: T, b: T): bool where T: INumber<T> => a <= b; "
        "  public static method Ge<T>(a: T, b: T): bool where T: INumber<T> => a >= b; "
        "end F; "
        "class C public static method Main(): int "
        "  using var a := new Meters(3); "
        "  using var b := new Meters(4); "
        "  using var c := new Meters(4); "
        "  var r := 0; "
        "  if F.Lt(a, b) then r := r + 1; end; "        // 3 < 4  → true
        "  if not F.Gt(a, b) then r := r + 2; end; "    // 3 > 4  → false
        "  if F.Le(a, b) then r := r + 4; end; "        // 3 <= 4 → true
        "  if not F.Ge(a, b) then r := r + 8; end; "    // 3 >= 4 → false
        "  if F.Le(b, c) then r := r + 16; end; "       // 4 <= 4 → true
        "  if F.Ge(b, c) then r := r + 32; end; "       // 4 >= 4 → true
        "  return r; "
        "end Main; end C;"), 63);
}

BEZEL_TEST(static_iface_dispatch_807, group_gt_int_bridge_1498) {
    // per §25.2/§3.5: the complete-group entitlement over a PRIMITIVE T — `>`
    // resolves to int's intrinsic relational (the compiler-provided §2.19
    // conformance). `Max2(9, 4)` → 9.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F "
        "  public static method Max2<T>(a: T, b: T): T where T: INumber<T> "
        "    if a > b then return a; end; return b; "
        "  end Max2; "
        "end F; "
        "class C public static method Main(): int "
        "  return F.Max2(9, 4); "
        "end Main; end C;"), 9);
}

BEZEL_TEST(static_iface_dispatch_807, ordering_without_comparison_constraint_rejected_1498) {
    // per §25.2: a type parameter declares no operators of its own, and its
    // candidate set is drawn ONLY from its static-interface constraints. A
    // constraint (IParsable<T>) that declares no comparison operator entitles
    // `>` to NOTHING — a clean compile error, NOT a fall-through to a primitive
    // `icmp` that crashes on a non-primitive instantiation (the #1498 abort).
    BEZEL_EXPECT_TRUE(rejects(
        "module M; static class F "
        "  public static method Gt<T>(a: T, b: T): bool where T: IParsable<T> "
        "    return a > b; "
        "  end Gt; "
        "end F; "
        "class C public static method Main(): int return 0; end Main; end C;"));
}

// ── §11.8 / §2.19: the primitive bridge, member by member ───────────────────

BEZEL_TEST(static_iface_dispatch_807, zero_one_bridge_int_double) {
    // INumber.Zero()/One() lower to the constants for primitive T.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F "
        "  public static method ZO<T>(): T where T: INumber<T> "
        "    return T.One() + T.One() + T.Zero(); "
        "  end ZO; "
        "end F; "
        "class C public static method Main(): int "
        "  var i := F.ZO<int>(); "
        "  var d := F.ZO<double>(); "
        "  return i * 10 + int(d); "
        "end Main; end C;"), 22);
}

BEZEL_TEST(static_iface_dispatch_807, arithmetic_bridge_int) {
    // +, -, *, / through the constraint on T = int (intrinsic §3.5 ops).
    BEZEL_EXPECT_EQ(run(
        "module M; static class F "
        "  public static method Calc<T>(a: T, b: T): T where T: INumber<T> "
        "    return (a + b) * (a - b) / b; "
        "  end Calc; "
        "end F; "
        "class C public static method Main(): int "
        "  return F.Calc(7, 3); "
        "end Main; end C;"), 13);  // (10 * 4) / 3 = 13 (integer division)
}

BEZEL_TEST(static_iface_dispatch_807, arithmetic_bridge_double) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F "
        "  public static method Calc<T>(a: T, b: T): T where T: INumber<T> "
        "    return (a + b) * (a - b) / b; "
        "  end Calc; "
        "end F; "
        "class C public static method Main(): int "
        "  return int(F.Calc(7.0, 3.5));  "
        "end Main; end C;"), 10);  // (10.5 * 3.5) / 3.5 = 10.5 → int 10
}

BEZEL_TEST(static_iface_dispatch_807, decimal_bridge) {
    // per §2.19: decimal conforms to all three; + routes via bz_decimal_add.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F "
        "  public static method Add2<T>(a: T, b: T): T where T: INumber<T> "
        "    return a + b; "
        "  end Add2; "
        "end F; "
        "class C public static method Main(): int "
        "  var d := F.Add2(1.5m, 2.25m); "
        "  if d = 3.75m then return 1; end; return 0; "
        "end Main; end C;"), 1);
}

BEZEL_TEST(static_iface_dispatch_807, step_bridge_char) {
    // ISteppable.Step for char: advance the code point ('a' + 2 = 'c').
    BEZEL_EXPECT_EQ(run(
        "module M; static class F "
        "  public static method Nxt<T>(v: T, by: long): T "
        "      where T: ISteppable<T> "
        "    return T.Step(v, by); "
        "  end Nxt; "
        "end F; "
        "class C public static method Main(): int "
        "  var c := F.Nxt('a', 2L); "
        "  if c = 'c' then return 1; end; return 0; "
        "end Main; end C;"), 1);
}

BEZEL_TEST(static_iface_dispatch_807, step_bridge_int_double) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F "
        "  public static method Nxt<T>(v: T, by: long): T "
        "      where T: ISteppable<T> "
        "    return T.Step(v, by); "
        "  end Nxt; "
        "end F; "
        "class C public static method Main(): int "
        "  var i := F.Nxt(40, 1L); "
        "  var d := F.Nxt(1.5, 2L); "
        "  return i + int(d * 2.0); "
        "end Main; end C;"), 48);  // 41 + int(3.5*2) = 41 + 7
}

BEZEL_TEST(static_iface_dispatch_807, parse_bridge_int_double) {
    // IParsable.Parse routes to the existing runtime parse lowerings.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F "
        "  public static method P<T>(s: string): T where T: IParsable<T> "
        "    return T.Parse(s); "
        "  end P; "
        "end F; "
        "class C public static method Main(): int "
        "  var i := F.P<int>(\"29\"); "
        "  var d := F.P<double>(\"2.5\"); "
        "  return i + int(d * 2.0); "
        "end Main; end C;"), 34);
}

BEZEL_TEST(static_iface_dispatch_807, parse_bridge_char) {
    // char.Parse via IParsable<char>: a one-code-point string.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F "
        "  public static method P<T>(s: string): T where T: IParsable<T> "
        "    return T.Parse(s); "
        "  end P; "
        "end F; "
        "class C public static method Main(): int "
        "  var c := F.P<char>(\"z\"); "
        "  if c = 'z' then return 1; end; return 0; "
        "end Main; end C;"), 1);
}

BEZEL_TEST(static_iface_dispatch_807, tryparse_bridge_nullable_out) {
    // TryParse in a generic body (the v0.171 out shape): success and failure.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F "
        "  public static method Try<T>(s: string, fallback: T): T "
        "      where T: IParsable<T> "
        "    if T.TryParse(s, out var v) then return v; end; "
        "    return fallback; "
        "  end Try; "
        "end F; "
        "class C public static method Main(): int "
        "  var a := F.Try(\"25\", 0); "
        "  var b := F.Try(\"xy\", 7); "
        "  return a + b; "
        "end Main; end C;"), 32);
}

BEZEL_TEST(static_iface_dispatch_807, tryparse_bridge_byte_range) {
    // byte.TryParse via the bridge: in-range parses, out-of-range → false.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F "
        "  public static method Try<T>(s: string, fallback: T): T "
        "      where T: IParsable<T> "
        "    if T.TryParse(s, out var v) then return v; end; "
        "    return fallback; "
        "  end Try; "
        "end F; "
        "class C public static method Main(): int "
        "  var c := F.Try(\"250\", byte(9)); "
        "  var d := F.Try(\"999\", byte(3)); "
        "  return int(c) + int(d); "
        "end Main; end C;"), 253);
}

// §11.8/§22.4/§1585 (guards the #1597-regression fix): the `when true`
// out-nullability postcondition on `IParsable<T>.TryParse` narrows `v: T?`
// non-null ONLY inside the controlled `then` branch (stdlib §1's Try
// convention, normative for the §11.8 `TryParse` companions). It must NOT
// leak past the `if`: a path exists on which the call was `false` (or, under
// §11.6 short-circuit DA, did not run at all), so `v` is still `T?` there and
// returning it as non-nullable `T` is a §7 error. This pins the narrowing
// scope so the checkAssignable generic-type-parameter unwrap can never
// over-narrow an un-narrowed `T?`.
BEZEL_TEST(static_iface_dispatch_807, tryparse_bridge_narrowing_scoped_to_then) {
    BEZEL_EXPECT_TRUE(rejects(
        "module M; static class F "
        "  public static method Try<T>(s: string): T "
        "      where T: IParsable<T> "
        "    if T.TryParse(s, out var v) then end; "
        "    return v; "
        "  end Try; "
        "end F; "
        "class C public static method Main(): int => 0; end C;"));
}

// ── §11.8: user-type Parse/TryParse (direct static calls, no bridge) ────────

BEZEL_TEST(static_iface_dispatch_807, user_parse_meters) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Meters: IParsable<Meters> "
        "  public Value: int; "
        "  public constructor(v: int) this.Value := v; end; "
        "  public static method Parse(s: string): Meters => "
        "    new Meters(int.Parse(s)); "
        "  public static method TryParse(s: string, out value: Meters?): bool "
        "    if int.TryParse(s, out var n) then "
        "      value := new Meters(n); return true; "
        "    end; "
        "    value := null; return false; "
        "  end TryParse; "
        "end Meters; "
        "static class F "
        "  public static method P<T>(s: string): T where T: IParsable<T> "
        "    return T.Parse(s); "
        "  end P; "
        "end F; "
        "class C public static method Main(): int "
        "  var m := F.P<Meters>(\"37\"); "
        "  return m.Value; "
        "end Main; end C;"), 37);
}

// ── §11.8: mixed / structural shapes ────────────────────────────────────────

// A generic method calling ANOTHER generic method, threading the constraint.
BEZEL_TEST(static_iface_dispatch_807, generic_calls_generic_threading) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F "
        "  public static method Add2<T>(a: T, b: T): T where T: INumber<T> "
        "    return a + b; "
        "  end Add2; "
        "  public static method Quad<T>(x: T): T where T: INumber<T> "
        "    return Add2(Add2(x, x), Add2(x, x)); "
        "  end Quad; "
        "end F; "
        "class C public static method Main(): int "
        "  return F.Quad(10) + int(F.Quad(0.25) * 4.0); "
        "end Main; end C;"), 44);  // Quad(10)=40; Quad(0.25)=1.0; 40 + 4
}

// A CLASS-level type parameter's constraint drives T.Member in method bodies.
BEZEL_TEST(static_iface_dispatch_807, class_level_constraint) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Box<T> where T: INumber<T> "
        "  public Item: T; "
        "  public constructor(v: T) this.Item := v; end; "
        "  public method DoubleIt(): T return this.Item + this.Item; end DoubleIt; "
        "  public method BaseZero(): T return T.Zero(); end BaseZero; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<int>(21); "
        "  return b.DoubleIt() + b.BaseZero(); "
        "end Main; end C;"), 42);
}

// ── §11.8: rejects — outside a constraint everything stays an error ─────────

BEZEL_TEST(static_iface_dispatch_807, unconstrained_t_plus_t_rejected) {
    BEZEL_EXPECT_TRUE(rejects(
        "module M; static class F "
        "  public static method Add2<T>(a: T, b: T): T "
        "    return a + b; "
        "  end Add2; "
        "end F; "
        "class C public static method Main(): int return 0; end Main; end C;"));
}

BEZEL_TEST(static_iface_dispatch_807, t_member_without_constraint_rejected) {
    BEZEL_EXPECT_TRUE(rejects(
        "module M; static class F "
        "  public static method Z<T>(): int "
        "    return T.Ignore(); "
        "  end Z; "
        "end F; "
        "class C public static method Main(): int return 0; end Main; end C;"));
}

BEZEL_TEST(static_iface_dispatch_807, nonconforming_string_rejected) {
    // per §2.19: string conforms to none — Sum over Array<string> rejects.
    BEZEL_EXPECT_TRUE(rejects(
        std::string("module M; ") + kSum +
        "class C public static method Main(): int "
        "  using var xs := new Array<string>(); "
        "  xs.Add(\"a\"); "
        "  var t := F.Sum(xs); return 0; "
        "end Main; end C;"));
}

BEZEL_TEST(static_iface_dispatch_807, char_not_inumber_rejected) {
    // per §2.19: char conforms to ISteppable/IParsable but NOT INumber.
    BEZEL_EXPECT_TRUE(rejects(
        "module M; static class F "
        "  public static method Add2<T>(a: T, b: T): T where T: INumber<T> "
        "    return a + b; "
        "  end Add2; "
        "end F; "
        "class C public static method Main(): int "
        "  var c := F.Add2('a', 'b'); return 0; "
        "end Main; end C;"));
}

// ── Non-generic code is unaffected (guard) ──────────────────────────────────

BEZEL_TEST(static_iface_dispatch_807, non_generic_operator_overload_unaffected) {
    // A plain user operator overload (no interfaces, no constraints) still
    // resolves through the ordinary §25 path.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Money "
        "  public Cents: int; "
        "  public constructor(c: int) this.Cents := c; end; "
        "  public static method operator +(a: Money, b: Money): Money => "
        "    new Money(a.Cents + b.Cents); "
        "end Money; "
        "class C public static method Main(): int "
        "  using var a := new Money(30); "
        "  using var b := new Money(12); "
        "  var s := a + b; "
        "  return s.Cents; "
        "end Main; end C;"), 42);
}

BEZEL_TEST(static_iface_dispatch_807, non_generic_int_arithmetic_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := 6; var b := 7; return a * b; "
        "end Main; end C;"), 42);
}
