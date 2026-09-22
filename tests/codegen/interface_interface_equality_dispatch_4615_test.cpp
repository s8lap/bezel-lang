// Codegen — issue #4615: `Interface = Interface` must answer §8.4's
// `a.Equals(b)` question, not reference identity, when both operands are
// statically INTERFACE-typed (or one is interface-typed and the other the
// concrete implementing class) (§8.4/§25.2/§11.2, v0.307).
//
// AUTHORITY.
//   §8.4 *`=` and `<>`* — "`a = b` invokes `a.Equals(b)`" — stated with no
//   exception for a statically-interface-typed operand.
//   §25.2 *Restrictions and resolution* — "`=` and `<>` may not be
//   overloaded on classes... For classes, `=` always invokes `Equals`" —
//   unconditional. An interface-typed operand's RUNTIME value is always a
//   class instance: interfaces cannot be instantiated on their own (§11.2:
//   "A class implements an interface implicitly: by declaring matching
//   public members"), so this rule applies to it exactly as it does to a
//   concretely-typed or `Object`-typed operand — there is no carve-out keyed
//   on the operand's *static* type, only on the runtime kind.
//
// PRECEDENT. This is the direct sibling of fix #4560 (`Object = Object`),
// which widened the identical `lowerBinary` `=`/`<>` predicate to accept
// `Object` but deliberately left `TypeDeclKind::Interface` untouched, filing
// this defect separately (#4615). Fix #4594 established the identical
// reasoning one funnel over, for an INTERFACE-typed *collection element*'s
// `Equals` (`Array<I>.Contains`/`.IndexOf`/Set/Dictionary dedup, via
// `emitRemoveElementEquality`): "every class's vtable slot 4 is
// unconditionally filled ... so routing an interface through
// `lowerObjectEquals` is total, not a fast path."
//
// THE BUG. `lowerBinary`'s `=`/`<>` dispatch arm routed to `lowerObjectEquals`
// (the vtable `kEqualsSlot` dispatch `.Equals()` already uses) only when both
// operands were `TypeRef::Kind::Named` with `declaration->kind ==
// TypeDeclKind::Class`. An interface type is ALSO `Kind::Named`, but its
// `declaration->kind` is `TypeDeclKind::Interface`, not `Class` — so any
// operand pair involving an interface-typed operand missed this arm entirely
// and fell through to CG4's raw `ICmpEQ`/`ICmpNE` below — reference identity —
// even though `.Equals()` on the identical pair already dispatched virtually
// through the very same `lowerObjectEquals` helper.
//
// MEASURED pre-fix (this repo's HEAD before #4615): two distinct instances of
// a class implementing an interface, with a real `Equals` override, structurally
// equal, compared as `I = I`, answered `False` (raw pointer compare) while
// `.Equals` on the same pair answered `True` (vtable dispatch to the override).
// A no-override class kept identity on both sides (unaffected — see the
// negative controls).
//
// THE FIX. Widen the #4560 predicate's per-operand test to also accept
// `TypeDeclKind::Interface`, folded into the existing `isObjectOrInterfaceTarget`
// helper (already Object+Interface, and already used identically at the
// `.Equals()` dispatch arm a few hundred lines above in the same file).
//
// FAIL-WITHOUT proven mechanically: with LlvmBackend_Exprs.cpp's #4615 hunk
// reverted (restored from origin/main) and the test binary rebuilt, every row
// below marked "pre-fix: answered X" flips to X; the negative controls (marked
// "unaffected by #4615") stay green on both sides, proving this test pins the
// bug and not the neighbourhood.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── the issue's own repro: two interface-typed locals over a class WITH an
//    Equals override, distinct instances, structurally equal ────────────────

// §8.4 (#4615): two distinct instances of a class overriding `Equals`,
// reached through TWO interface-typed locals, must answer `=` true — the
// dispatched override, not reference identity. Pre-fix: answered false (raw
// pointer compare on the two distinct `K` instances).
BEZEL_TEST(interface_interface_equality_dispatch_4615,
           interface_eq_interface_with_override_is_true_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; interface I method Get(): int; end I; "
        "class K: I public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public method Get(): int return V; end Get; "
        "  public override method Equals(o: Object?): bool "
        "    var other: K? := o as K; "
        "    if other = null then return false; end; "
        "    return other.V = this.V; "
        "  end Equals; "
        "  public override method GetHashCode(): int return V; end GetHashCode; "
        "end K; "
        "class C public static method Main(): int "
        "  var a: I := new K(7); "
        "  var b: I := new K(7); "
        "  if (a = b) and (a.Equals(b)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.4: a field mismatch under the same override must still answer false via
// `=` (the override runs the real comparison, not a stub) — guards against an
// over-eager fix that just returns `true` unconditionally.
BEZEL_TEST(interface_interface_equality_dispatch_4615,
           interface_eq_interface_with_override_differing_fields_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; interface I method Get(): int; end I; "
        "class K: I public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public method Get(): int return V; end Get; "
        "  public override method Equals(o: Object?): bool "
        "    var other: K? := o as K; "
        "    if other = null then return false; end; "
        "    return other.V = this.V; "
        "  end Equals; "
        "  public override method GetHashCode(): int return V; end GetHashCode; "
        "end K; "
        "class C public static method Main(): int "
        "  var a: I := new K(7); "
        "  var b: I := new K(9); "
        "  if a = b then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §8.4: `<>` is the negation — two Equals-equal interface-typed instances
// must NOT be `<>`. Pre-fix: answered true (raw pointer compare said the
// distinct instances were unequal).
BEZEL_TEST(interface_interface_equality_dispatch_4615,
           interface_ne_interface_with_override_equal_is_false_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; interface I method Get(): int; end I; "
        "class K: I public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public method Get(): int return V; end Get; "
        "  public override method Equals(o: Object?): bool "
        "    var other: K? := o as K; "
        "    if other = null then return false; end; "
        "    return other.V = this.V; "
        "  end Equals; "
        "  public override method GetHashCode(): int return V; end GetHashCode; "
        "end K; "
        "class C public static method Main(): int "
        "  var a: I := new K(7); "
        "  var b: I := new K(7); "
        "  if a <> b then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ── mixed pair: interface-typed on one side, concrete-class-typed on the
//    other, same root cause and same code path as the both-interface case ───

// §8.4 (#4615): `I = K` (interface on the LEFT — the §8.4 receiver position).
// Pre-fix: answered false, the identical bug as the both-interface case, via
// the same predicate (an interface-typed operand never satisfied the old
// `Kind::Named && Class` test).
BEZEL_TEST(interface_interface_equality_dispatch_4615,
           mixed_interface_left_class_right_with_override_is_true_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; interface I method Get(): int; end I; "
        "class K: I public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public method Get(): int return V; end Get; "
        "  public override method Equals(o: Object?): bool "
        "    var other: K? := o as K; "
        "    if other = null then return false; end; "
        "    return other.V = this.V; "
        "  end Equals; "
        "  public override method GetHashCode(): int return V; end GetHashCode; "
        "end K; "
        "class C public static method Main(): int "
        "  var i: I := new K(3); "
        "  var k: K := new K(3); "
        "  if i = k then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.4: the mirror order, `K = I` (class on the LEFT). Pre-fix: also
// answered false.
BEZEL_TEST(interface_interface_equality_dispatch_4615,
           mixed_class_left_interface_right_with_override_is_true_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; interface I method Get(): int; end I; "
        "class K: I public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public method Get(): int return V; end Get; "
        "  public override method Equals(o: Object?): bool "
        "    var other: K? := o as K; "
        "    if other = null then return false; end; "
        "    return other.V = this.V; "
        "  end Equals; "
        "  public override method GetHashCode(): int return V; end GetHashCode; "
        "end K; "
        "class C public static method Main(): int "
        "  var k: K := new K(3); "
        "  var i: I := new K(3); "
        "  if k = i then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── negative controls: a class with NO Equals override is unaffected in
//    every shape, because reference identity IS its §8.3 default — the fix
//    must not turn every interface comparison into a spurious `true` ───────

// §8.3 (control): both-interface, no override, distinct instances — identity
// and dispatch agree (false either way). Unaffected by #4615.
BEZEL_TEST(interface_interface_equality_dispatch_4615,
           interface_eq_interface_no_override_distinct_is_false_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; interface I method Get(): int; end I; "
        "class Plain: I public method Get(): int return 1; end Get; end Plain; "
        "class C public static method Main(): int "
        "  var a: I := new Plain(); "
        "  var b: I := new Plain(); "
        "  if a = b then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §8.3 (control): both-interface, no override, SAME instance — identity and
// dispatch agree (true either way). Unaffected by #4615.
BEZEL_TEST(interface_interface_equality_dispatch_4615,
           interface_eq_interface_no_override_same_instance_is_true_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; interface I method Get(): int; end I; "
        "class Plain: I public method Get(): int return 1; end Get; end Plain; "
        "class C public static method Main(): int "
        "  var a: I := new Plain(); "
        "  var b: I := a; "
        "  if a = b then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.3 (control): mixed interface/class pair, no override, distinct
// instances — identity and dispatch agree (false either way). Unaffected by
// #4615.
BEZEL_TEST(interface_interface_equality_dispatch_4615,
           mixed_no_override_distinct_is_false_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; interface I method Get(): int; end I; "
        "class Plain: I public method Get(): int return 1; end Get; end Plain; "
        "class C public static method Main(): int "
        "  var a: I := new Plain(); "
        "  var b: Plain := new Plain(); "
        "  if a = b then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §8.3 (control): `<>` with no override, distinct instances — must stay true
// (reference identity says they differ). Unaffected by #4615.
BEZEL_TEST(interface_interface_equality_dispatch_4615,
           interface_ne_interface_no_override_distinct_is_true_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; interface I method Get(): int; end I; "
        "class Plain: I public method Get(): int return 1; end Get; end Plain; "
        "class C public static method Main(): int "
        "  var a: I := new Plain(); "
        "  var b: I := new Plain(); "
        "  if a <> b then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}
