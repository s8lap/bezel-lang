// Codegen — issue #4560: `Object = Object` must answer §8.4's `a.Equals(b)`
// question, not reference identity, when both operands are statically
// `Object` (or one is `Object` and the other a concrete class) (§8.1/§8.3/
// §8.4/§25.2, v0.306).
//
// AUTHORITY.
//   §8.1 *Definition* — `Object`'s member surface includes
//   `public virtual method Equals(other: Object?): bool`. `Object` is itself
//   `public class Object`.
//   §8.3 *Equals and GetHashCode* — the compiler-generated default `Equals`
//   is "Reference equality" for a Class and "Value comparison" for a
//   Primitive; a boxed primitive's Equals thunk implements the latter.
//   §8.4 *`=` and `<>`* — "`a = b` invokes `a.Equals(b)`, with a null-safe
//   wrapper" — stated with no exception for a statically-`Object`-typed
//   operand.
//   §25.2 *Restrictions and resolution* — "`=` and `<>` may not be
//   overloaded on classes... For classes, `=` always invokes `Equals`" —
//   unconditional, and `Object` is a class (§8.1).
//
// THE BUG. `lowerBinary`'s #1106 arm dispatched `=`/`<>` through
// `lowerObjectEquals` (vtable `kEqualsSlot`, the same routing `.Equals()`
// calls use) only when BOTH operands were `TypeRef::Kind::Named` with
// `declaration->kind == TypeDeclKind::Class`. `Object` is represented as
// `TypeRef::Kind::Primitive`/`PrimitiveType::Object` (so `is`/`as`/boxing can
// treat it uniformly with the true primitives — see #3903's root-cause note),
// so ANY pair with an `Object`-typed operand missed this arm entirely and
// fell through to CG4's raw `ICmpEQ`/`ICmpNE` — reference identity — even
// though `.Equals()` on the identical pair already dispatched virtually
// through the very same `lowerObjectEquals` helper.
//
// MEASURED pre-fix (this repo's HEAD before #4560, both via the issue's own
// repro and this file's own rows before the fix): `(Object)5 = (Object)5`
// answered `False` (two distinct boxes, compared by raw pointer) while
// `((Object)5).Equals((Object)5)` on the same values answered `True` (vtable
// dispatch to the box's value-comparison override) — and identically for a
// user class WITH an `Equals` override reached through `Object`-typed
// locals, in both mixed-typed orders (`Object = C` and `C = Object`).
//
// THE FIX. Widen the #1106 predicate's per-operand test to also accept a
// statically-`Object`-typed operand (`Kind::Primitive`/`PrimitiveType::
// Object`) alongside the existing `Kind::Named`/`TypeDeclKind::Class` test.
// Because the predicate is applied to each operand independently and then
// ANDed, this covers `Object = Object` (the issue's own repro) and the mixed
// `Object = someClass` pair (both orders) as the same, single code change —
// not two separate fixes. A class with NO `Equals` override is unaffected in
// every shape (its default IS reference equality, so identity and dispatch
// agree) — see the negative controls below. Interfaces are deliberately
// UNCHANGED by this fix (a distinct, separately-tracked defect).
//
// FAIL-WITHOUT proven mechanically: with LlvmBackend_Exprs.cpp's #4560 hunk
// reverted (restored from the pre-fix commit) and the test binary rebuilt,
// every row below marked "pre-fix: answered X" flips to X; the negative
// controls (marked "unaffected by #4560") stay green on both sides, proving
// this test pins the bug and not the neighbourhood.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── the issue's own repro: two boxed ints, both operands statically Object ──

// §8.3/§8.4 (#4560): two distinct boxes of the SAME int value are `Equals`-
// equal (§8.3: a primitive's default Equals is value comparison), so `=`
// (§8.4: invokes `Equals`) must answer true. Pre-fix: answered false (raw
// pointer ICmpEQ on the two distinct box addresses).
BEZEL_TEST(object_object_equality_dispatch_4560,
           object_eq_object_boxed_int_equal_is_true_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 5; "
        "  var p: Object := 5; "
        "  if o = p then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.4: the positive form's mirror — unequal payloads stay unequal, so this
// row would also pass under an over-eager fix that just returned `true`
// unconditionally; paired with the row above it pins the actual VALUE
// comparison, not a constant.
BEZEL_TEST(object_object_equality_dispatch_4560,
           object_eq_object_boxed_int_unequal_is_false_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 5; "
        "  var p: Object := 6; "
        "  if o = p then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §8.4: `<>` is the negation — equal boxed ints must NOT be `<>`. Pre-fix:
// answered true (raw pointer compare said the distinct boxes were unequal).
BEZEL_TEST(object_object_equality_dispatch_4560,
           object_ne_object_boxed_int_equal_is_false_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 5; "
        "  var p: Object := 5; "
        "  if o <> p then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ── class WITH an Equals override, reached through Object-typed locals ──────

// §8.3/§8.4 (#4560): a user class overriding `Equals` reached through TWO
// `Object`-typed locals must answer structurally through `=`, exactly as
// `.Equals()` already does. Pre-fix: answered false (raw pointer compare on
// the two distinct `Val` instances).
BEZEL_TEST(object_object_equality_dispatch_4560,
           object_eq_object_class_with_override_is_true_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Val public n: int; "
        "  public constructor(v: int) this.n := v; end constructor; "
        "  public override method Equals(other: Object?): bool "
        "    var s := other as Val; "
        "    if s = null then return false; end; "
        "    return s.n = this.n; "
        "  end Equals; "
        "  public override method GetHashCode(): int => this.n; "
        "end Val; "
        "class C public static method Main(): int "
        "  var o: Object := new Val(7); "
        "  var p: Object := new Val(7); "
        "  if (o = p) and (o.Equals(p)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.3/§8.4: a field mismatch under the same override must still answer
// false via `=` (the override runs the real comparison, not a stub).
BEZEL_TEST(object_object_equality_dispatch_4560,
           object_eq_object_class_with_override_differing_fields_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Val public n: int; "
        "  public constructor(v: int) this.n := v; end constructor; "
        "  public override method Equals(other: Object?): bool "
        "    var s := other as Val; "
        "    if s = null then return false; end; "
        "    return s.n = this.n; "
        "  end Equals; "
        "  public override method GetHashCode(): int => this.n; "
        "end Val; "
        "class C public static method Main(): int "
        "  var o: Object := new Val(7); "
        "  var p: Object := new Val(9); "
        "  if o = p then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ── mixed pair: Object-typed on one side, concrete-class-typed on the other,
//    same root cause and same code path as the both-Object case above ──────

// §8.3/§8.4 (#4560): `Object = C` (Object on the LEFT — the §8.4 receiver
// position). Pre-fix: answered false, the identical bug as the both-Object
// case, via the same predicate (an Object-typed operand never satisfied the
// old `Kind::Named && Class` test on EITHER side).
BEZEL_TEST(object_object_equality_dispatch_4560,
           mixed_object_left_class_right_with_override_is_true_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Val public n: int; "
        "  public constructor(v: int) this.n := v; end constructor; "
        "  public override method Equals(other: Object?): bool "
        "    var s := other as Val; "
        "    if s = null then return false; end; "
        "    return s.n = this.n; "
        "  end Equals; "
        "  public override method GetHashCode(): int => this.n; "
        "end Val; "
        "class C public static method Main(): int "
        "  var o: Object := new Val(3); "
        "  var v: Val := new Val(3); "
        "  if o = v then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.3/§8.4: the mirror order, `C = Object` (class on the LEFT). Pre-fix:
// also answered false.
BEZEL_TEST(object_object_equality_dispatch_4560,
           mixed_class_left_object_right_with_override_is_true_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Val public n: int; "
        "  public constructor(v: int) this.n := v; end constructor; "
        "  public override method Equals(other: Object?): bool "
        "    var s := other as Val; "
        "    if s = null then return false; end; "
        "    return s.n = this.n; "
        "  end Equals; "
        "  public override method GetHashCode(): int => this.n; "
        "end Val; "
        "class C public static method Main(): int "
        "  var v: Val := new Val(3); "
        "  var o: Object := new Val(3); "
        "  if v = o then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── negative controls: a class with NO Equals override is unaffected in
//    every shape, because reference identity IS its §8.3 default — the fix
//    must not turn every class comparison into a spurious `true` ──────────

// §8.3 (control): both-Object, no override, distinct instances — identity
// and dispatch agree (false either way). Unaffected by #4560.
BEZEL_TEST(object_object_equality_dispatch_4560,
           object_eq_object_no_override_distinct_is_false_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Plain end Plain; "
        "class C public static method Main(): int "
        "  var a: Object := new Plain(); "
        "  var b: Object := new Plain(); "
        "  if a = b then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §8.3 (control): both-Object, no override, SAME instance — identity and
// dispatch agree (true either way). Unaffected by #4560.
BEZEL_TEST(object_object_equality_dispatch_4560,
           object_eq_object_no_override_same_instance_is_true_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Plain end Plain; "
        "class C public static method Main(): int "
        "  var a: Object := new Plain(); "
        "  var b: Object := a; "
        "  if a = b then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.3 (control): mixed Object/class pair, no override, distinct instances —
// identity and dispatch agree (false either way). Unaffected by #4560.
BEZEL_TEST(object_object_equality_dispatch_4560,
           mixed_no_override_distinct_is_false_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Plain end Plain; "
        "class C public static method Main(): int "
        "  var a: Object := new Plain(); "
        "  var b: Plain := new Plain(); "
        "  if a = b then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ── Array<Object>.Contains: already correct (fixed by #4573), pinned here
//    as a control so a future regression on the element-equality funnel
//    (a SEPARATE code path from lowerBinary's `=`/`<>`) is not mistaken for
//    this issue's fix. Both rows below use a PRE-BOXED `Object`-typed local
//    for the search argument — exactly #4573's own tested shape
//    (element_equality_sema_dispatch_4573_test.cpp's
//    array_object_boxed_value_equality_spec_8_4: "var present: Object := 2;")
//    — deliberately, NOT a bare `int` argument. A bare-`int` argument to
//    `Array<Object>.Contains` was measured (while authoring this file) to
//    always answer `false` regardless of presence — e.g.
//    `Array<Object>{1,2}.Contains(1)` (1 clearly present) answers `false` —
//    a genuine, separate defect in argument boxing at the `Contains` call
//    site, unrelated to lowerBinary's `=`/`<>` operator this fix touches (the
//    pre-boxed-argument path above is unaffected and correct). Out of #4560's
//    scope; filed separately as #4619. ────────────────────────────────────

BEZEL_TEST(object_object_equality_dispatch_4560,
           array_object_contains_boxed_object_absent_control_4573) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<Object> := [1, 2]; "
        "  var absent: Object := 5; "
        "  if a.Contains(absent) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

BEZEL_TEST(object_object_equality_dispatch_4560,
           array_object_contains_boxed_object_present_control_4573) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<Object> := [1, 2]; "
        "  var present: Object := 1; "
        "  if a.Contains(present) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}
