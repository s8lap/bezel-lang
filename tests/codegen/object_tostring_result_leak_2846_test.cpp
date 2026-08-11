// Spec-derived regression test for issue #2846.
//
// §8.5 (`ToString`) / §8.6 (Boxing) / §28.9 (Runtime-managed types), v0.240.
//
// §8.6 states, verbatim:
//
//     "When a value type is used where `Object` (or an interface implemented by
//      the value type) is expected, the value is **boxed**: a heap-allocated
//      wrapper holds the value. Boxed values are runtime-managed ([§28.9])."
//
// and §28.9 leaves the mechanism open but not the obligation:
//
//     "The implementation of runtime-managed types' lifecycle (reference
//      counting, region allocation, or other) is not part of this
//      specification."
//
// §8.5 gives every value a `ToString` and fixes what each kind produces; the
// `string` it produces is itself runtime-managed, so it must be reclaimed.
//
// SYMPTOM (measured on b0fa41a). `o.ToString()` on an `Object`-typed receiver
// leaked ONE string PER CALL — unbounded in a loop. The issue's own repro (a
// boxed record, 20 iterations) reported "913 byte(s) leaked in 38 allocations"
// from `bz$boxtostr$Pt`; a boxed `int` leaked from `bz$primtostr$int`.
//
// ROOT CAUSE, and a CORRECTION to the issue's. The issue attributes the leak to
// the box thunks and records "a CLASS instance through `Object` does NOT leak —
// the class path releases its result; only the BOX ToString family does not."
// That contrast is real but its explanation is not: NOTHING released the result
// on any path. `isPlusOneStringProducer` (LlvmBackend_Setup.cpp) deliberately
// EXCLUDED `Object.ToString()` — "goes through lowerObjectToString which may or
// may not allocate — excluded for safety" — so `var s := o.ToString();` treated
// the result as a BORROW: the bind retained on top of the producer's own
// reference and the producer's +1 was never dropped. The class case merely does
// not ALLOCATE: §8.5 says a class's default `ToString` is "the type's qualified
// name", which lowers to an immortal string literal. Give the class a `ToString`
// override that MINTS and it leaks exactly like the boxes — MEASURED: 711 bytes
// / 40 allocations from `bz$App.KMint$ToString` over 20 iterations. T4 pins that
// shape, which the issue believed was already correct.
//
// THE FIX has two halves and needs both:
//   1. `lowerObjectToString`'s STRING arm (LlvmBackend_String.cpp) returned the
//      receiver's own pointer UN-RETAINED, breaking the +1 contract every
//      caller of that function already assumed (`emitPrimitiveToString`'s
//      declaration calls it "allocates a fresh heap string with rc := 1";
//      `StringBuilder.Append(o: Object)` sets `rendered = true` and TRANSFERS).
//      That was a live HEAP-USE-AFTER-FREE, not a leak — T6 is its oracle. It is
//      also what made half 2 unsound before it: recognizing the call as a
//      producer without it turns a balanced retain/release into a bare release.
//   2. `isPlusOneStringProducer` now recognizes `ToString()` on an `Object`
//      receiver with no resolved MethodSymbol — the exact gate
//      `lowerObjectToString`'s call site uses (`isObjectRecv`, under `if (!ms)`,
//      LlvmBackend_Calls2.cpp).
//
// EVIDENCE DISCIPLINE. Every leak case is LOOP-AMPLIFIED (20 iterations plus a
// churn allocation): a one-shot `Main` is not a reliable LSan oracle, because a
// leftover pointer in a register makes LSan report a leaked block as merely
// reachable. Strings are built at RUNTIME (`"t" + i.ToString()`), never folded
// from two literals — a literal is an immortal sentinel on which release is a
// no-op, which would make the probe vacuous. And the direction that a pure
// "no leak" assertion is blind to — OVER-release — is asserted separately, by
// reading the still-live string AFTER the calls under generated-code ASan
// (T5/T6): an over-release frees it and the read faults.
//
// Deliberately NOT covered here: an INTERFACE-, class- or builtin-exception-
// typed static receiver reaches the same lowering through the sibling gates and
// keeps the pre-existing leak; that widening is filed separately rather than
// made unmeasured.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

int run(const bezel::test::JitResult& r) { return r.ran ? r.exitCode : -999; }

std::string out(const bezel::test::JitResult& r) {
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

}  // namespace

// ---------------------------------------------------------------------------
// THE ISSUE'S OWN REPRO, under the ASan/LSan net.
// ---------------------------------------------------------------------------

// #2846 / §8.6 + §28.9 — a boxed RECORD's ToString through `Object`. 20
// iterations; pre-fix LSan reported 19-20 leaked strings from `bz$boxtostr$Pt`.
// The `s` re-read after the churn allocation is the over-release half: were the
// result released once too often, that read would fault under generated-code
// ASan rather than pass silently.
BEZEL_TEST(codegen_object_tostring_result_leak_2846,
           boxed_record_tostring_result_is_released_once_8_6) {
    auto r = runAotChecked(
        "module App; "
        "record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var t: int := 0; "
        "  var i: int := 0; "
        "  while i < 20 do "
        "    i := i + 1; "
        "    var p: Pt := new Pt(i, i); "
        "    var o: Object := p; "
        "    var s: string := o.ToString(); "
        "    var churn: string := \"c\" + i.ToString(); "
        "    t := t + s.Length + churn.Length; "
        "  end while; "
        "  if t <> 353 then return 1; end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
}

// #2846 / §8.6 "the value is boxed" — the boxed PRIMITIVE grid. `decimal` is
// there on purpose: #2793 widened box-thunk minting to it, so the leak now
// applies to more types than when the issue was filed. `bool` is the one
// primitive whose §8.5 rendering is a sentinel ("True"/"False", stdlib
// bool.ToString) — it never leaked and must not now over-release.
BEZEL_TEST(codegen_object_tostring_result_leak_2846,
           boxed_primitive_tostring_results_are_released_once_8_6) {
    auto r = runAotChecked(
        "module App; "
        "class C public static method Main(): int "
        "  var t: int := 0; "
        "  var i: int := 0; "
        "  while i < 20 do "
        "    i := i + 1; "
        "    var oi: Object := i; "
        "    var od: Object := 1.5; "
        "    var oc: Object := 'z'; "
        "    var ob: Object := true; "
        "    var om: Object := 2.5m; "
        "    t := t + oi.ToString().Length + od.ToString().Length "
        "           + oc.ToString().Length + ob.ToString().Length "
        "           + om.ToString().Length; "
        "    var churn: string := \"c\" + i.ToString(); "
        "    t := t + churn.Length; "
        "  end while; "
        "  if t < 1 then return 1; end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
}

// #2846 / §8.5 — the boxed ENUM pair. §8.5: "for enums: the type's qualified
// name. An enum may replace its default by declaring a plain `ToString():
// string` member (§10.5); the replacement is used by every consumer, including
// interpolation and boxed dispatch." The default is a sentinel (no leak either
// way — the CONTROL); the replacement MINTS and leaked.
BEZEL_TEST(codegen_object_tostring_result_leak_2846,
           boxed_enum_default_and_override_8_5) {
    auto r = runAotChecked(
        "module App; "
        "enum Color Red, Green; end Color; "
        "enum Tag A, B; "
        "  public method ToString(): string "
        "    return \"T\" + \"g\".Substring(0, 1); "
        "  end ToString; "
        "end Tag; "
        "class C public static method Main(): int "
        "  var t: int := 0; "
        "  var i: int := 0; "
        "  while i < 20 do "
        "    i := i + 1; "
        "    var oe: Object := Color.Green; "
        "    var og: Object := Tag.B; "
        "    t := t + oe.ToString().Length + og.ToString().Length; "
        "    var churn: string := \"c\" + i.ToString(); "
        "    t := t + churn.Length; "
        "  end while; "
        "  if t < 1 then return 1; end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
}

// #2846 / §8.5 "For classes: the type's qualified name" — the CLASS half, and
// the correction to the issue's root cause. `KDef` (no override) never leaked
// because its answer is an immortal literal — that is the issue's "the class
// path is correct" observation, kept here as the control. `KMint` (an override
// that MINTS) leaked exactly like the boxes: 711 bytes / 40 allocations over 20
// iterations, from `bz$App.KMint$ToString`.
BEZEL_TEST(codegen_object_tostring_result_leak_2846,
           class_default_and_minting_override_through_object_8_5) {
    auto r = runAotChecked(
        "module App; "
        "class KDef public N: int := 0; end KDef; "
        "class KMint "
        "  public N: int := 0; "
        "  public override method ToString(): string "
        "    return \"K\" + N.ToString(); "
        "  end ToString; "
        "end KMint; "
        "class C public static method Main(): int "
        "  var t: int := 0; "
        "  var i: int := 0; "
        "  while i < 20 do "
        "    i := i + 1; "
        "    using var kd := new KDef(); "
        "    using var km := new KMint(); "
        "    km.N := i; "
        "    var o1: Object := kd; "
        "    var o2: Object := km; "
        "    t := t + o1.ToString().Length + o2.ToString().Length; "
        "    var churn: string := \"c\" + i.ToString(); "
        "    t := t + churn.Length; "
        "  end while; "
        "  if t < 1 then return 1; end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
}

// #2846 / §8.5 "via virtual dispatch" + §11.2 — an INTERFACE-typed receiver
// reaches the same `lowerObjectToString` lowering through the sibling gate
// (`isIfaceRecv`, LlvmBackend_Calls2.cpp) and leaked the same way. Both
// override shapes are here, and the second is the load-bearing over-release
// oracle: `Rc.ToString()` returns a stored FIELD, so the value the caller
// releases is one the object still holds — the callee's §28.3 return mint is
// the only thing that makes the release balanced. `rc.Name` is read AFTER the
// 20 rounds, so an over-release faults under generated-code ASan instead of
// passing quietly. The class with NO override (a §8.5 qualified-name literal)
// is the third arm.
BEZEL_TEST(codegen_object_tostring_result_leak_2846,
           interface_receiver_and_field_returning_override_8_5) {
    auto r = runAotChecked(
        "module App; "
        "interface IShape method Area(): int; end IShape; "
        "class Sq: IShape "
        "  public N: int := 0; "
        "  public method Area(): int => N * N; "
        "  public override method ToString(): string "
        "    return \"S\" + N.ToString(); "
        "  end ToString; "
        "end Sq; "
        "class Rc: IShape "
        "  public Name: string := \"r\"; "
        "  public method Area(): int => 1; "
        "  public override method ToString(): string "
        "    return Name; "
        "  end ToString; "
        "end Rc; "
        "class Plain public N: int := 0; end Plain; "
        "class C public static method Main(): int "
        "  var t: int := 0; "
        "  var i: int := 0; "
        "  using var rc := new Rc(); "
        "  rc.Name := \"n\" + \"ame\".Substring(0, 3); "
        "  while i < 20 do "
        "    i := i + 1; "
        "    using var sq := new Sq(); "
        "    sq.N := i; "
        "    var a: IShape := sq; "
        "    var b: IShape := rc; "
        "    using var p := new Plain(); "
        "    t := t + a.ToString().Length + b.ToString().Length "
        "           + p.ToString().Length; "
        "    var churn: string := \"c\" + i.ToString(); "
        "    t := t + churn.Length; "
        "  end while; "
        "  if rc.Name <> \"name\" then return 2; end if; "
        "  if t <> 282 then return 3; end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
}

// ---------------------------------------------------------------------------
// THE OVER-RELEASE DIRECTION. `runAotCheckedLenient` = generated-code ASan with
// LSan off: these two programs assert that nothing is freed too early, and are
// deliberately not leak-asserted (binding a `string` local into an `Object`
// local marks it escaping, so its own +1 is left un-released by design —
// armStringLocalRefcount's "safe direction" — and would mask the signal).
// ---------------------------------------------------------------------------

// #2846 / §9.6 (`string.ToString` is identity) — the STRING arm of
// `lowerObjectToString`. The receiver's own pointer is what comes back, so the
// value must survive the caller's release. 20 rounds with a churn allocation so
// a freed block is likely reused, then the source string is read AND printed
// after the loop: an over-release faults, a wrong value shows in stdout.
BEZEL_TEST(codegen_object_tostring_result_leak_2846,
           object_tostring_on_a_string_receiver_does_not_free_it_9_6) {
    auto r = runAotCheckedLenient(
        "module App; "
        "class C public static method Main(): int "
        "  var t: int := 0; "
        "  var s: string := \"ab\" + \"cd\".Substring(0, 2); "
        "  var i: int := 0; "
        "  while i < 20 do "
        "    i := i + 1; "
        "    var o: Object := s; "
        "    var r: string := o.ToString(); "
        "    var churn: string := \"c\" + i.ToString(); "
        "    t := t + r.Length + churn.Length; "
        "  end while; "
        "  Console.WriteLine(s); "
        "  Console.WriteLine(t); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
    BEZEL_EXPECT_TRUE(out(r) == "abcd\n131\n");
}

// #2846 (companion half) / §18.5 — INTERPOLATION of an `Object` hole holding a
// string. This is the shape the missing retain made a live
// heap-use-after-free: the interpolation render path TRANSFERS
// `lowerObjectToString`'s result (it counts as "rendered"), so it released a
// string it did not own. MEASURED on b0fa41a: "AddressSanitizer:
// heap-use-after-free … freed by bz_string_release" on the second iteration's
// read of `s`.
BEZEL_TEST(codegen_object_tostring_result_leak_2846,
           interpolating_an_object_hole_holding_a_string_18_5) {
    auto r = runAotCheckedLenient(
        "module App; "
        "class C public static method Main(): int "
        "  var t: int := 0; "
        "  var s: string := \"ab\" + \"cd\".Substring(0, 2); "
        "  var i: int := 0; "
        "  while i < 20 do "
        "    i := i + 1; "
        "    var o: Object := s; "
        "    var r: string := $\"[{o}]\"; "
        "    var churn: string := \"c\" + i.ToString(); "
        "    t := t + r.Length + churn.Length; "
        "  end while; "
        "  Console.WriteLine(s); "
        "  Console.WriteLine(t); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
    BEZEL_EXPECT_TRUE(out(r) == "abcd\n171\n");
}

// ---------------------------------------------------------------------------
// VALUE CONTROLS — the fix is refcount bookkeeping and must not change a single
// rendered character. §8.5's per-kind table, read back through `Object`.
// ---------------------------------------------------------------------------

BEZEL_TEST(codegen_object_tostring_result_leak_2846,
           rendered_text_is_unchanged_8_5) {
    auto r = out(runAot(
        "module App; "
        "record Pt(X: int, Y: int); "
        "enum Color Red, Green; end Color; "
        "class KDef public N: int := 0; end KDef; "
        "class KMint "
        "  public override method ToString(): string "
        "    return \"K\" + \"9\".Substring(0, 1); "
        "  end ToString; "
        "end KMint; "
        "class C public static method Main(): int "
        "  using var kd := new KDef(); using var km := new KMint(); "
        "  var s: string := \"ab\" + \"cd\".Substring(0, 2); "
        "  var o1: Object := new Pt(1, 2); "
        "  var o2: Object := 7; "
        "  var o3: Object := Color.Green; "
        "  var o4: Object := kd; "
        "  var o5: Object := km; "
        "  var o6: Object := s; "
        "  Console.WriteLine(o1.ToString()); "
        "  Console.WriteLine(o2.ToString()); "
        "  Console.WriteLine(o3.ToString()); "
        "  Console.WriteLine(o4.ToString()); "
        "  Console.WriteLine(o5.ToString()); "
        "  Console.WriteLine(o6.ToString()); "
        "  return 0; "
        "end Main; end C;"));
    BEZEL_EXPECT_TRUE(r == "Pt(X: 1, Y: 2)\n7\nColor\nKDef\nK9\nabcd\n");
}
