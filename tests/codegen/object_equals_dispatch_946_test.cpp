// Codegen — Object.Equals / Object.GetHashCode dispatch through an Object- or
// interface-typed receiver (§9.6). Issues #946, #947, #948.
//
// §9.6 declares the Object root members `ToString()`, `Equals(other: Object?):
// bool` and `GetHashCode(): int` as virtual — every type may override them, and
// an override must be reachable through an Object-typed receiver. ToString
// already occupied a fixed vtable slot; Equals and GetHashCode did not, so a
// call on an Object-typed receiver could not reach the override:
//   • #946 — `ox.Equals(oy)` (ox, oy: Object) behaved as reference equality even
//     when the dynamic type (string, or a user class with an `override Equals`)
//     defines content/structural equality.
//   • #947 — the unreached call produced an i32 result, so an inferred
//     `var r := o.Equals(x)` typed `r` as i32 and `not r` emitted `xor i32,-1`
//     as a branch condition → LLVM IR-verification failure.
//   • #948 — `o.GetHashCode()` through Object did not reach string's content
//     hash (distinct hashes for content-equal strings).
//
// The fix adds two fixed vtable slots (Equals at kEqualsSlot=4, GetHashCode at
// kGetHashCodeSlot=5; user virtuals shift to kVirtualMethodsBase=6) wired by
// per-class thunks (user override else §8.3 reference-identity / identity-hash),
// and an Object/interface dispatch (lowerObjectEquals / lowerObjectGetHashCode)
// that branches on bz_obj_is_string: a string uses the content intrinsic, a
// class/box dispatches through the fixed slot. Equals returns i1 so `not r` is
// valid IR.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// A class P(x) with the §8.3 idiom: `override Equals(Object?)` downcasts with
// `as P`, null-checks, compares the `x` field; `override GetHashCode` returns a
// content hash. Main body is spliced in.
int runWithP(const std::string& mainBody) {
    return run(
        "module M; "
        "class P "
        "  public x: int; "
        "  public constructor(v: int) this.x := v; end constructor; "
        "  public override method Equals(other: Object?): bool "
        "    var p := other as P; "
        "    if p = null then return false; end; "
        "    return p.x = this.x; "
        "  end Equals; "
        "  public override method GetHashCode(): int => this.x; "
        "end P; "
        "class C public static method Main(): int " + mainBody +
        " end Main; end C;");
}

} // namespace

// #946 / §9.6: two content-equal but distinct strings compared through Object
// receivers use string's content equality (the override), so Equals is true.
BEZEL_TEST(codegen_object_equals_946, string_content_equality_through_object) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: string := \"hel\" + \"lo\"; "
        "  var y: string := \"he\" + \"llo\"; "
        "  var ox: Object := x; var oy: Object := y; "
        "  return (1 if ox.Equals(oy) else 0); "
        "end Main; end C;"), 1);
}

// #946 / §9.6: a user class's `override Equals(Object?)` is reached through an
// Object-typed receiver — two P(5) values are equal by field comparison.
BEZEL_TEST(codegen_object_equals_946, user_override_reached_through_object) {
    BEZEL_EXPECT_EQ(runWithP(
        "using var a := new P(5); using var b := new P(5); "
        "var oa: Object := a; var ob: Object := b; "
        "return (1 if oa.Equals(ob) else 0);"), 1);
}

// #946 / §8.3: a user override is reached through Object and a field mismatch
// makes the override return false (not a spurious reference-equality true).
BEZEL_TEST(codegen_object_equals_946, user_override_differing_fields_false) {
    BEZEL_EXPECT_EQ(runWithP(
        "using var a := new P(5); using var b := new P(9); "
        "var oa: Object := a; var ob: Object := b; "
        "return (1 if oa.Equals(ob) else 0);"), 0);
}

// #946 / §8.3: a class WITHOUT an Equals override keeps reference identity
// through Object — two distinct objects are not equal, the same object is.
BEZEL_TEST(codegen_object_equals_946, no_override_is_reference_identity) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Q public x: int; "
        "  public constructor(v: int) this.x := v; end constructor; end Q; "
        "class C public static method Main(): int "
        "  using var a := new Q(5); using var b := new Q(5); "
        "  var oa: Object := a; var ob: Object := b; "
        "  if oa.Equals(ob) then return 9; end; "
        "  return (1 if oa.Equals(oa) else 0); "
        "end Main; end C;"), 1);
}

// #946 / §9.6: an override is reachable through an INTERFACE-typed receiver too
// (the same uniform vtable slot).
BEZEL_TEST(codegen_object_equals_946, override_reached_through_interface) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IShape end IShape; "
        "class Sq : IShape "
        "  public n: int; "
        "  public constructor(v: int) this.n := v; end constructor; "
        "  public override method Equals(other: Object?): bool "
        "    var s := other as Sq; "
        "    if s = null then return false; end; "
        "    return s.n = this.n; "
        "  end Equals; "
        "  public override method GetHashCode(): int => this.n; "
        "end Sq; "
        "class C public static method Main(): int "
        "  using var a := new Sq(4); using var b := new Sq(4); "
        "  var ia: IShape := a; var ib: IShape := b; "
        "  return (1 if ia.Equals(ib) else 0); "
        "end Main; end C;"), 1);
}

// #947 / §9.6: an inferred `var r := o.Equals(x)` is bool-typed (the dispatch
// returns i1), so `not r` lowers to a valid i1 branch condition rather than the
// `xor i32,-1` that previously failed IR verification. s equals itself → r is
// true → `not r` is false → the function returns 1 (and, critically, compiles).
BEZEL_TEST(codegen_object_equals_947, not_on_inferred_equals_result_is_valid_ir) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: string := \"hello\"; var os: Object := s; "
        "  var r := os.Equals(s as Object); "
        "  if not r then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// #948 / §9.6: string's content hash is reached through an Object receiver, so
// two content-equal strings hash equal both directly and through Object.
BEZEL_TEST(codegen_object_hashcode_948, string_content_hash_through_object) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: string := \"hello\"; var b: string := \"hel\" + \"lo\"; "
        "  var oa: Object := a; var ob: Object := b; "
        "  if (a.GetHashCode() = b.GetHashCode()) "
        "       and (oa.GetHashCode() = ob.GetHashCode()) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// #948 / §9.6: a user `override GetHashCode()` is reached through an Object
// receiver — P(2).GetHashCode() returns 2 via the override.
BEZEL_TEST(codegen_object_hashcode_948, user_override_reached_through_object) {
    BEZEL_EXPECT_EQ(runWithP(
        "using var a := new P(2); var oa: Object := a; "
        "return oa.GetHashCode();"), 2);
}

// ── #1119: Object.ReferenceEquals(a, b) (§8.1/§8.4) ──────────────────────────
// Pointer-identity compare; was rejected as a nonexistent static method.

// Same reference → true; two distinct objects → false.
BEZEL_TEST(codegen_reference_equals_1119, identity_true_distinct_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class N public v: int := 0; end N; "
        "class C public static method Main(): int "
        "  using var a := new N(); using var b := new N(); "
        "  var acc := 0; "
        "  if Object.ReferenceEquals(a, a) then acc := acc + 1; end; "
        "  if Object.ReferenceEquals(a, b) then acc := acc + 10; end; "
        "  return acc; "                                  // 1 (same ref only)
        "end Main; end C;"), 1);
}

// §8.4: ReferenceEquals IGNORES an Equals override — P(5) and P(5) are
// Equals-equal but distinct references, so ReferenceEquals is false.
BEZEL_TEST(codegen_reference_equals_1119, ignores_equals_override) {
    BEZEL_EXPECT_EQ(runWithP(
        "using var a := new P(5); using var b := new P(5); "
        "var acc := 0; "
        "if a.Equals(b) then acc := acc + 1; end; "               // +1: override
        "if Object.ReferenceEquals(a, b) then acc := acc + 10; end; "  // +0: distinct
        "return acc;"), 1);
}

// A null reference compares equal to another null.
BEZEL_TEST(codegen_reference_equals_1119, null_equals_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class N public v: int := 0; end N; "
        "class C public static method Main(): int "
        "  var a: N? := null; "
        "  return (1 if Object.ReferenceEquals(a, null) else 0); "
        "end Main; end C;"), 1);
}

// §8.4: passing value-type arguments always returns false (each boxing is a
// fresh object).
BEZEL_TEST(codegen_reference_equals_1119, value_type_args_always_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (1 if Object.ReferenceEquals(5, 5) else 0); "
        "end Main; end C;"), 0);
}

// An object reached through both its class type and an interface type is the
// same reference → true.
BEZEL_TEST(codegen_reference_equals_1119, class_and_interface_ref_same_object) {
    BEZEL_EXPECT_EQ(run(
        "module M; interface IShape method Area(): int; end IShape; "
        "class Sq: IShape public method Area(): int => 4; end Sq; "
        "class C public static method Main(): int "
        "  using var s := new Sq(); var i: IShape := s; "
        "  return (1 if Object.ReferenceEquals(s, i) else 0); "
        "end Main; end C;"), 1);
}

// ── #1151 / §8.3-§8.4: a VALUE-typed `Equals(other: int)` is NOT the Object ────
// override and must not be bound into the uniform i1(ptr,ptr) slot-4 thunk.
// Before the fix the classEqualsThunk/classEqualsOverride lookups accepted ANY
// 1-param `Equals`, so a class with `Equals(other: int)` (e.g. an IEquatable<int>
// contract impl) passed the thunk's ptr argument to the method's i32 parameter
// → "Call parameter type does not match" IR-verification crash, even with no
// interface present. The fix guards the override lookup on the compiled param[1]
// being a pointer; a value-typed Equals fills only its own (later, itable) slot.

// #1151: a class with a value-typed Equals(int) and NO interface compiles + the
// method is directly callable (was an IR-fail).
BEZEL_TEST(codegen_object_equals_946, value_typed_equals_int_compiles_1151) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public method Equals(other: int): bool => other > 3; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return (5 if c.Equals(7) else 0); "          // true → 5
        "  end Main; end C;"), 5);
}

// #1151: a class implementing the (decl-less) builtin IEquatable<int> with the
// contract Equals(int) compiles (the implements-clause IR-fail was this same
// Equals-thunk confound, not the interface itself).
BEZEL_TEST(codegen_object_equals_946, class_implements_iequatable_int_compiles_1151) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C: IEquatable<int> "
        "  public method Equals(other: int): bool => true; "
        "  public static method Main(): int "
        "    using var m := new C(); return 5; "
        "  end Main; end C;"), 5);
}

// #1151 keep-green: a real `Equals(Object?)` override (pointer param) IS still
// bound to the Object.Equals slot-4 thunk and dispatches through `=` / `.Equals`.
BEZEL_TEST(codegen_object_equals_946, object_param_equals_still_overrides_1151) {
    BEZEL_EXPECT_EQ(run(
        "module M; class P "
        "  public override method Equals(other: Object?): bool => true; "
        "  public static method Main(): int "
        "    using var a := new P(); using var b := new P(); "
        "    return (7 if a.Equals(b) else 0) + (3 if a = b else 0); "  // 10
        "  end Main; end P;"), 10);
}
