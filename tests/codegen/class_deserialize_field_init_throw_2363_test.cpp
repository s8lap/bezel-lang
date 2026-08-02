// Regression tests for #2363 — the synthesized PRIMARY-CONSTRUCTOR class
// `Deserialize` path (LlvmBackend_Serialization.cpp's `emitClassDeserialize`,
// the `primaryCtorUsed` branch #2325 added) called `emitOwnFieldInit(ts, obj,
// st)` directly with none of the throw-safety cleanup that the equivalent
// no-explicit-ctor `new`-expression construction site
// (LlvmBackend_Exprs2.cpp's `!hasAnyConstructor(t)` branch, ~line 4344)
// wraps around the identical call. If an OWN field initializer that runs
// AFTER an earlier owned/string field's initializer has already completed
// throws, the earlier field's value — and the partially-constructed instance
// block itself — leaked, because nothing disposed them before the exception
// propagated out of `emitClassDeserialize`.
//
// Authority:
//   - §9.8 (v0.234) "Synthesized Deserialize": "each owned object is
//     constructed by an ordinary call to its constructor" — the primary
//     constructor is a constructor for this purpose (§9.8's own
//     "Constructor selection" paragraph, and #2325's comment in
//     LlvmBackend_Serialization.cpp), so ordinary constructor exception
//     safety governs its throw edges too.
//   - §22.9 (v0.234) "Owned fields and exception safety": "An exception
//     during a field initializer (step 2) triggers disposal of any earlier
//     derived owned fields whose initializers completed, in reverse
//     declaration order, followed by the base subobject's Dispose() ... The
//     exception then propagates" and "the derived instance is never
//     observable." §22.9 also states primary-ctor CAPTURE fields (step 0,
//     §22.8) "participate in every case above as owned fields whose
//     initialization completed first."
//   - §28.3 (v0.234) "Exception-path discharge": "Owned locals and owned
//     parameters that are live when an exception propagates ... are disposed
//     implicitly during stack unwinding ... as if wrapped in an implicit
//     try/finally whose finally disposes the value if it is still owned."
//
// The fix mirrors the exact cleanup shape of LlvmBackend_Exprs2.cpp's
// `!hasAnyConstructor(t)` branch (`emitOwnedFieldDisposals` +
// `emitBaseDisposeChain` + `free(obj)`, routed via a temporary
// `unwindTarget_` override) into `emitClassDeserialize`'s primary-ctor path,
// around its `emitOwnFieldInit` call.
//
// Test-construction note: every throwing scenario below builds its wire
// bytes via an ACTUAL `Serialize()` call on a real, non-throwing Bezel
// object (never hand-rolled `BinarySerializer` token calls with a guessed
// field name/case) — the throwing "Second" component is driven to 0 by
// INTEGER DIVISION on the ORIGINAL construction (`100 / 200 == 0`), so the
// serialized wire genuinely contains `Second = 0`; deserializing then
// re-runs `Second`'s own field initializer (`100 / second`), which divides
// by that wire-supplied 0 and throws `DivideByZeroException` mid-
// construction, after `First`'s initializer has already completed.
//
// This file also pins two PRE-EXISTING (#2325 / #550) wire-reading-loop
// truncation-safety scenarios explicitly requested by the task brief —
// these exercise code this fix does NOT touch (the truncation cleanup at
// LlvmBackend_Serialization.cpp lines ~2780-2841 runs entirely BEFORE the
// primary-ctor construction path, before `calloc`), kept here as regression
// confirmation that the new field-init-throw cleanup composes cleanly with
// the pre-existing wire-truncation cleanup rather than as evidence of a new
// fix.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// runAotChecked returns ran=false on any sanitizer fire (SEGV / leak / UAF);
// map that to a sentinel so a single EXPECT_EQ pins BOTH memory-cleanliness
// AND the round-tripped/exit value (mirrors
// class_primary_ctor_serialization_2325_test.cpp's runChecked).
int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

const char* kImport = "import System.Serialization; ";

// Shared class declarations for the field-init-throw scenarios: `First` (an
// owned nested-class component) is a plain transfer — its initializer never
// throws and always completes first (declaration order). `Second`'s
// initializer (`100 / second`) recomputes from the wire-supplied `second`
// component on EVERY deserialize, so it throws DivideByZeroException iff the
// wire's `Second` value is 0 (integer division of 100 by anything >100
// truncates to 0 at construction time, letting a normal, non-throwing build
// produce a wire-zero without ever constructing a throwing instance).
const char* kLeafAndDanger =
    "class Leaf(v: int): ISerializable, IDeserializable "
    "  public property V: int => v; "
    "end Leaf; "
    "class Danger(owned first: Leaf, second: int): ISerializable, IDeserializable "
    "  public owned First: Leaf := first; "
    "  public Second: int := 100 / second; "
    "end Danger; ";

}  // namespace

// ===========================================================================
// Primary fix target: an OWN field initializer that runs AFTER an earlier
// owned/string field's initializer completed throws during Deserialize.
// ===========================================================================

// Control: a clean round-trip with a NON-zero `second` component must stay
// green — the new cleanup code must not alter behavior on the non-throwing
// path (the `cdeser.pcfield.cleanup` block it introduces is never branched
// to). `100 / 5 == 20` at the original construction; on deserialize,
// `Second`'s initializer re-runs against the wire value 20, yielding
// `100 / 20 == 5` (the field's initializer is a genuine recomputation, not a
// passthrough, so the round-trip is intentionally not value-preserving —
// only that it completes without throwing or leaking is asserted here).
BEZEL_TEST(codegen_class_deserialize_field_init_throw_2363,
           clean_roundtrip_control_no_throw_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport + kLeafAndDanger +
        "class C public static method Main(): int "
        "  using var dOrig := new Danger(new Leaf(9), 5); "
        "  if dOrig.Second <> 20 then return 90; end; "
        "  using var s := new BinarySerializer(); "
        "  dOrig.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var d2 := Danger.Deserialize(d); "
        "  if d2.First.V <> 9 then return 91; end; "
        "  return (if d2.Second = 5 then 42 else 92); "
        "end Main; end C;"), 42);
}

// The primary repro: `First` (owned class-typed field) completes its
// initializer (a plain transfer of the deserialized primary-ctor capture),
// THEN `Second`'s initializer (`100 / second`) throws DivideByZeroException
// because the wire-supplied `second` component is 0 (the original instance
// was built with `second = 200`, so `100 / 200 == 0` at construction —
// integer division — and THAT zero is what gets serialized and read back).
// Per §22.9, `First` — "an earlier derived owned field whose initializer
// completed" — must be disposed before the exception propagates. Pre-fix:
// leaked (no cleanup existed around `emitOwnFieldInit` on this path at all).
// runAotChecked (ASan/LSan) is the failure oracle: `ran=false` (mapped to
// -999) on any leak or memory-safety violation.
BEZEL_TEST(codegen_class_deserialize_field_init_throw_2363,
           owned_field_disposed_when_later_field_init_throws_spec_9_8_22_9) {
    const std::string kProg =
        std::string("module M; ") + kImport + kLeafAndDanger +
        "class C public static method Main(): int "
        "  using var dOrig := new Danger(new Leaf(9), 200); "
        "  if dOrig.Second <> 0 then return 90; end; "
        "  using var s := new BinarySerializer(); "
        "  dOrig.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var caught: bool := false; "
        "  try "
        "    using var d2 := Danger.Deserialize(d); "
        "    return 0; "
        "  catch DivideByZeroException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;";
    // Exit-code leg: DivideByZeroException is caught; First's leak (if any)
    // is invisible to a plain exit-code check.
    BEZEL_EXPECT_EQ(run(kProg), 42);
    // ASan/LSan leg: this is the actual failure oracle for #2363 — pre-fix,
    // this leg reports `ran=false` (LSan detects the leaked First/Leaf
    // instance), mapped to the -999 sentinel.
    BEZEL_EXPECT_EQ(runChecked(kProg), 42);
}

// Same shape, but the EARLIER field is a STRING rather than an owned class —
// §4.6/§28.9 string fields are released by the SAME emitOwnedFieldDisposals
// pass (the "release every STRING field" block, LlvmBackend_Disposal.cpp),
// so the cleanup this fix adds must dispose that too, not just class-typed
// owned fields. Also exercises the double-free self-pass concern: the
// primary-ctor capture slot for `first` (referenced only in `Name`'s
// initializer, an INITIALIZER position, so NOT captured per §22.10's
// body-vs-initializer capture rule) must not ALSO be independently disposed
// — only the explicit `Name` field (the genuine owned/independent +1) is a
// member of `emitOwnedFieldDisposals`'s "string field" set.
BEZEL_TEST(codegen_class_deserialize_field_init_throw_2363,
           string_field_disposed_when_later_field_init_throws_spec_9_8_22_9) {
    const std::string kProg =
        std::string("module M; ") + kImport +
        "class Danger(name: string, second: int): ISerializable, IDeserializable "
        "  public Name: string := name; "
        "  public Second: int := 100 / second; "
        "end Danger; "
        "class C public static method Main(): int "
        "  using var dOrig := new Danger(\"leaked-if-broken\", 200); "
        "  if dOrig.Second <> 0 then return 90; end; "
        "  using var s := new BinarySerializer(); "
        "  dOrig.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var caught: bool := false; "
        "  try "
        "    using var d2 := Danger.Deserialize(d); "
        "    return 0; "
        "  catch DivideByZeroException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(kProg), 42);
    BEZEL_EXPECT_EQ(runChecked(kProg), 42);
}

// Repeated throw/dispose cycles — a leak-per-iteration bug would show up as
// a large, clearly-nonzero leak report rather than a single missed free that
// a one-shot test might mask under LSan's allocator noise floor (mirrors
// class_primary_ctor_serialization_2325_test.cpp's
// repeated_roundtrip_no_accumulating_leak_spec_9_8).
BEZEL_TEST(codegen_class_deserialize_field_init_throw_2363,
           repeated_throwing_roundtrip_no_accumulating_leak_spec_9_8_22_9) {
    const std::string kProg =
        std::string("module M; ") + kImport + kLeafAndDanger +
        "class C public static method Main(): int "
        "  var i: int := 0; "
        "  var caughtCount: int := 0; "
        "  while i < 25 do "
        "    using var dOrig := new Danger(new Leaf(i), 200); "
        "    using var s := new BinarySerializer(); "
        "    dOrig.Serialize(s); "
        "    using var bytes := s.ToBytes(); "
        "    using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "    try "
        "      using var d2 := Danger.Deserialize(d); "
        "    catch DivideByZeroException => caughtCount := caughtCount + 1; end; "
        "    i := i + 1; "
        "  end; "
        "  return (if caughtCount = 25 then 42 else caughtCount); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(kProg), 42);
    BEZEL_EXPECT_EQ(runChecked(kProg), 42);
}

// ===========================================================================
// Nested eligible class component failing inside its OWN Deserialize — this
// exercises the PRE-EXISTING wire-reading-loop cleanup (#2325 / #550, lines
// ~2780-2841 of LlvmBackend_Serialization.cpp), not the new field-init-throw
// cleanup this fix adds (the nested Deserialize call happens entirely BEFORE
// `calloc` for the outer Order object) — kept as regression confirmation
// that the two cleanup mechanisms compose without interference.
// ===========================================================================

BEZEL_TEST(codegen_class_deserialize_field_init_throw_2363,
           nested_class_deserialize_failure_no_leak_of_outer_or_inner_spec_9_8) {
    const std::string kProg =
        std::string("module M; ") + kImport +
        "class Customer(name: string): ISerializable, IDeserializable "
        "  public property Name: string => name; "
        "end Customer; "
        "class Order(owned customer: Customer, extra: string): ISerializable, IDeserializable "
        "  public owned Customer: Customer := customer; "
        "  public Extra: string := extra; "
        "end Order; "
        "class C public static method Main(): int "
        "  using var o := new Order(new Customer(\"Ada\"), \"tag\"); "
        "  using var s := new BinarySerializer(); "
        "  o.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        // Truncate INSIDE the nested Customer object's own encoding: a short
        // prefix of the buffer includes the outer BeginObject + \"Customer\"
        // field name + the nested BeginObject, but cuts before Customer's
        // own field is fully readable — forces the RECURSIVE
        // emitClassDeserialize(Customer) call to raise SerializationException
        // from inside the OUTER wire-reading loop, before Order's own
        // `calloc` ever runs.
        "  var cutLen: int := bytes.Length / 4; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    using var o2 := Order.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(kProg), 42);
    BEZEL_EXPECT_EQ(runChecked(kProg), 42);
}

// ===========================================================================
// Wire-truncation with TWO owned/string components, truncated after the
// first — the task brief's literal request. Pre-existing #2325/#550
// coverage (class_primary_ctor_serialization_2325_test.cpp's
// truncated_input_no_leak_primary_ctor_nested_owned_spec_9_8 already covers
// this shape); kept here for direct regression confirmation alongside this
// fix's own tests, in the same file the fix touches.
//
// Field order note: the scalar/string component is declared FIRST and the
// owned class-typed component SECOND. While narrowing this down, a
// PRE-EXISTING, UNRELATED leak was found (filed as #2378 — a
// `bz_binder_read_int`/`bz_binder_read_string` truncation-detection path
// that leaks its own in-flight allocation, e.g. the exception object or the
// partially-read string, before raising `SerializationException`) that
// triggers specifically when the cut lands mid-primitive-value-read inside
// an EARLIER-declared owned class-typed field's own encoding. That leak is
// unrelated to #2363 (it fires entirely inside the pre-existing wire-
// reading loop, before this fix's `emitOwnFieldInit` cleanup is ever
// reached) so it is not exercised here; this test's field order and cut
// point were chosen (and verified empirically) to land cleanly at a
// field-name boundary instead, matching #2378's own non-leaking control
// shape (`serialization_tier2_classes_test.cpp`'s
// `truncated_input_asan_no_leak_spec_9_8_v0136`, which also declares its
// `owned` field first — so the trigger is offset-specific, not simply
// "owned field first").
// ===========================================================================

BEZEL_TEST(codegen_class_deserialize_field_init_throw_2363,
           truncated_after_first_of_two_owned_string_components_no_leak_spec_9_8_550) {
    const std::string kProg =
        std::string("module M; ") + kImport +
        "class Leaf(v: int): ISerializable, IDeserializable "
        "  public property V: int => v; "
        "end Leaf; "
        "class Pair(first: string, owned second: Leaf): ISerializable, IDeserializable "
        "  public First: string := first; "
        "  public owned Second: Leaf := second; "
        "end Pair; "
        "class C public static method Main(): int "
        "  using var p := new Pair(\"tag-value\", new Leaf(9)); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var halfLen: int := bytes.Length / 2; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, halfLen)); "
        "  var caught: bool := false; "
        "  try "
        "    using var back := Pair.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(kProg), 42);
    BEZEL_EXPECT_EQ(runChecked(kProg), 42);
}
