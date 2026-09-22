// Codegen — issue #4559: `v.Equals(o)` — a VALUE-typed receiver's explicit
// `.Equals(...)` method-call spelling — reached the primitive `IEquatable<T>`
// fast path (`#863`, immediately above in `LlvmBackend_Exprs.cpp`) whenever
// the receiver was int/FP-family, and that path SExt'd/FPExt'd whatever the
// argument lowered to WITHOUT checking it was actually int/FP-shaped:
//
//   SExt only operates on integer
//     %eq.rsx = sext ptr %4 to i32
//   bezelc: codegen error: IR verification failed
//
// AUTHORITY. §5.1's reference conversions include "any type to `Object`", a
// value type reaching it by boxing (§8.6). §8.1 fixes the universal
// `Equals(other: Object?): bool` every type answers, and §8.4 gives its
// null-safe meaning ("`null = null` is `true`; `null = nonNull` and
// `nonNull = null` are `false`"). §8.3 fixes what the dispatch actually
// compares: a box's `Equals` compares PAYLOAD stamped with its VTABLE, so two
// values of different runtime type are never equal even when one converts
// numerically to the other (the same "box carries its type" rule fix #4511
// established for `Object = value`, e.g. a boxed `string "5"` `<>` `int 5`).
//
// `Emitter::lowerValueSemUniversalEquals` (LlvmBackend_Calls2.cpp) already
// implements exactly that shape — box the receiver, dispatch through
// `lowerObjectEquals`, release the boxes — for `5.Equals(o)` (#3480) and every
// record/enum/choice receiver's `.Equals`. THE FIX routes this primitive
// method-call spelling to that same helper whenever the ARGUMENT's static
// type does not settle as the receiver's own numeric FAMILY (the shape the
// pre-existing #863 SExt/ZExt-widening fast path below is actually for —
// preserved unchanged for e.g. `int.Equals(someLong)`, which the pre-fix
// compiler already answered correctly and must keep answering).
//
// SCOPE. Measured the full argument-shape grid on `9d7450a9` (which already
// carries fix #4511, `Object = value`, the operator sibling of this issue):
//   int.Equals(Object)        -> CRASH  (SExt on a pointer)      -> fixed
//   bool.Equals(Object)       -> CRASH  (SExt on a pointer, i1)  -> fixed
//   double.Equals(Object)     -> CRASH  (FPExt on a pointer)     -> fixed
//   int.Equals(someClass)     -> CRASH  (SExt on a pointer)      -> fixed
//   int.Equals(someInterface) -> CRASH  (SExt on a pointer)      -> fixed
//   int.Equals("a string")    -> CRASH  (SExt on a pointer)      -> fixed
//   int.Equals(int?)          -> CRASH  (SExt on a {i8,i32})     -> fixed
//   double.Equals(int)        -> CRASH  (FPExt on an integer)    -> fixed
//   int.Equals(someLong)      -> already correct (#863 widening) -> UNCHANGED
//   o.Equals(i)  (reverse)    -> already correct (§9.6 / #946)   -> UNCHANGED
//   record.Equals(Object)     -> already correct (namedValueSem) -> UNCHANGED
// `decimal.Equals(...)` was found separately and independently BROKEN for
// every argument, including another `decimal` — a struct-shaped receiver the
// `lowerValueSemMethodOn` primitive arm never handles at all (silently
// returns nullptr, some caller further out answers a bare `false`). That is a
// different function, a pre-existing defect unrelated to argument boxing, and
// NOT fixed here — filed separately (issue #4562) rather than widening this
// lane, per CLAUDE.md's "no while-we're-at-it expansions".
//
// FAIL-WITHOUT proved mechanically: with LlvmBackend_Exprs.cpp restored from
// this worktree's pre-fix HEAD and the test binary rebuilt, every CRASH row
// above reports `ran == false` (the AOT harness's compile-time failure
// sentinel), so `run()` returns the -999 sentinel against an expected 0/1.
//
// §8.1 / §8.3 / §8.4 / §8.6 / §5.1, v0.306.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── the issue's own repro, both truth values ────────────────────────────────

// §8.1/§8.6: `i.Equals(o)` where `o` holds the same int value, boxed — the
// receiver is boxed too, and the two boxes compare payload-equal.
BEZEL_TEST(value_equals_object_arg_4559, int_equals_object_arg_true_spec_8_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 5; "
        "  var i: int := 5; "
        "  if i.Equals(o) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.3: the positive form of the SAME pair with unequal payloads answers
// false — the row a fix that returned a constant `true` would not survive.
BEZEL_TEST(value_equals_object_arg_4559, int_equals_object_arg_false_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 5; "
        "  var i: int := 9; "
        "  if i.Equals(o) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §8.1/§8.6: `bool.Equals(Object)` — a second primitive kind sharing the
// `isInt` family (Bool is in the same #863 arm as Int), both truth values.
BEZEL_TEST(value_equals_object_arg_4559, bool_equals_object_arg_spec_8_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := true; "
        "  var t: bool := true; "
        "  var f: bool := false; "
        "  if (t.Equals(o)) and not (f.Equals(o)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.1/§8.6: `double.Equals(Object)` — the FP family, previously `FPExt`'d a
// pointer ("FPExt only operates on FP").
BEZEL_TEST(value_equals_object_arg_4559, double_equals_object_arg_spec_8_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 5.5; "
        "  var d: double := 5.5; "
        "  if d.Equals(o) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── argument shapes beyond `Object` that the SAME broken guard admitted ────

// §8.1: a plain CLASS-typed argument (not `Object`, not an interface) reaches
// the universal member exactly the same way — `int` does not implement
// `IEquatable<Foo>`, so overload resolution can only be `Object.Equals`.
// Never equal (different runtime type entirely), and must not crash.
BEZEL_TEST(value_equals_object_arg_4559, int_equals_class_arg_false_spec_8_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Foo end Foo; "
        "class C public static method Main(): int "
        "  var f: Foo := new Foo(); "
        "  var i: int := 5; "
        "  if i.Equals(f) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §8.1: an INTERFACE-typed argument holding a boxed int — dynamically DOES
// carry the receiver's own type, so the comparison is true when the payloads
// match. Distinguishes a fix that merely returns `false` for every pointer.
// `IEquatable<int>` is the §2.19 compiler-provided conformance `int` itself
// satisfies (a hand-declared empty marker interface is NOT implicitly
// convertible from `int` — §5.1 rejects that at compile time, confirmed
// separately — so this is the interface `int` genuinely implements).
BEZEL_TEST(value_equals_object_arg_4559, int_equals_interface_arg_true_spec_8_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var n: IEquatable<int> := 5; "
        "  var i: int := 5; "
        "  if i.Equals(n) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §4.6/§8.3: a `string` argument — a REFERENCE type that lowers directly to a
// `bz_string*` (no box). Different runtime type than a boxed `int`, so the
// comparison is false, and the string's own pointer must not be SExt'd.
BEZEL_TEST(value_equals_object_arg_4559, int_equals_string_arg_false_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: string := \"5\"; "
        "  var i: int := 5; "
        "  if i.Equals(s) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §8.6 "Nullable value-type boxing": `int.Equals(int?)` — a PRESENT `int?`
// boxes to a box of its payload (equal payload -> true); an ABSENT one boxes
// to the null reference, which §8.4's null-safe wrapper makes unequal to a
// non-null receiver's box.
BEZEL_TEST(value_equals_object_arg_4559, int_equals_nullable_int_arg_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var i: int := 5; "
        "  var n: int? := 5; "
        "  var z: int? := null; "
        "  if (i.Equals(n)) and not (i.Equals(z)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.1/§8.3: the CROSS-FAMILY case — an FP receiver against an INT argument.
// `IEquatable<double>.Equals(double)` does not admit an `int`, so this is also
// the universal member: both sides box, to DIFFERENT vtables (double-box vs
// int-box), so the comparison is false — not the numeric-widening `true` the
// pre-fix arm's blind `FPExt` was attempting (and crashing on).
BEZEL_TEST(value_equals_object_arg_4559, double_equals_int_arg_false_spec_8_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: double := 5.0; "
        "  var i: int := 5; "
        "  if d.Equals(i) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ── controls: spellings that already worked must not move ──────────────────

// #863's own generic-substitution widening: `int.Equals(long)` reaches the
// SAME #863 fast path (both operands are `isInt`-family) and was already
// answering correctly pre-fix. Pinned so the new argument-family gate cannot
// silently reroute a same-family pair through the (slower, boxing) universal
// path and change its answer.
BEZEL_TEST(value_equals_object_arg_4559, int_equals_long_arg_unchanged_spec_863) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var i: int := 5; "
        "  var l: long := 5; "
        "  var m: long := 9; "
        "  if (i.Equals(l)) and not (i.Equals(m)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §9.6 / #946: the spelling that already worked before this fix — an
// Object-typed RECEIVER calling `.Equals` on a value-typed argument (the
// direction #4511/#3480 cover). Unchanged by this fix (a different function).
BEZEL_TEST(value_equals_object_arg_4559, object_receiver_equals_int_unchanged_spec_9_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 5; "
        "  var i: int := 5; "
        "  if o.Equals(i) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// A RECORD receiver's `.Equals(Object)` already worked pre-fix (it dispatches
// through `tryLowerValueSemanticsCall`'s `namedValueSem` branch, not the
// primitive arm this fix touches). Pinned as an UNCHANGED control.
BEZEL_TEST(value_equals_object_arg_4559, record_equals_object_arg_unchanged_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var o: Object := new P(1, 2); "
        "  var p: P := new P(1, 2); "
        "  var q: P := new P(9, 9); "
        "  if (p.Equals(o)) and not (p.Equals(q)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── the `where T: IEquatable<T>` generic face (#863) — both operands `T` ───

// This fix mirrors the #730 bare-type-parameter substitution onto the
// ARGUMENT's static type too (previously only applied to the receiver), so a
// generic body where BOTH `a` and `b` are the unsubstituted parameter `T`
// still resolves to the SAME concrete primitive family and takes the fast
// widening path, exactly as `tests/codegen/primitive_equals_constraint_863_
// test.cpp` already pins for several instantiations. This row adds an
// Object-argument grid point INSIDE that generic body, which the pre-existing
// #863 suite never exercised: T substitutes to `int`, and `b`'s STATIC type
// inside the generic body is the bare parameter, not `Object` — so this
// specifically exercises the argument-side substitution mirror.
BEZEL_TEST(value_equals_object_arg_4559, generic_t_bound_equals_still_dispatches_spec_863) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "static class U "
        "  public static method Eq<T>(a: T, b: T): bool where T: IEquatable<T> "
        "    return a.Equals(b); "
        "  end Eq; "
        "end U; "
        "class C public static method Main(): int "
        "  if (U.Eq(7, 7)) and not (U.Eq(7, 9)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── leak safety of the NEW call site (loop-amplified; a one-shot LSan row
// passes vacuously — see CLAUDE.md "Oracles that pass vacuously") ──────────

// §8.6/§28.9: `lowerValueSemUniversalEquals` mints and releases the
// RECEIVER's box itself (unchanged by this fix); this row is the new CALL
// SITE reaching it (a concrete primitive receiver's direct `.Equals(Object)`
// method-call spelling), looped x300 so a leak at this site is visible under
// ASan rather than passing on a live stack slot.
const char* kLoopedIntEqualsObject =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 300 do "
    "      var o: Object := 5; "
    "      var i: int := 5; "
    "      if i.Equals(o) then t := t + 1; end if; "
    "      k := k + 1; "
    "    end while; "
    "    if t = 300 then return 7; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

BEZEL_TEST(value_equals_object_arg_4559, looped_receiver_box_released_spec_8_6) {
    JitResult r = runAotChecked(kLoopedIntEqualsObject);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(value_equals_object_arg_4559, looped_receiver_plain_value_invariant_spec_8_1) {
    JitResult r = runAot(kLoopedIntEqualsObject);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
