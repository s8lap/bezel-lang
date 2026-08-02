// Codegen — issue #2447 / spec §35.9 ("Nullable `C?`") + §7.2 ("Nullable
// value types"), docs revision v0.235.
//
// §35.9: "`C?` is the ordinary nullable value type (§7.2)" — a nullable
// CHOICE must lower to the SAME tagged `{i8 hasValue; T}` struct every
// other nullable value type (int?, an enum?, a record?, a value-builtin
// like Guid?) already uses (isNullableValueTy / nullableValueStructTy,
// LlvmBackend_Layout.cpp). No predicate in that unification recognized a
// choice at all: `isNullableValueTy` was `isNullablePrimTy ||
// isNullableEnumTy || isNullableRecordTy || isNullableValueBuiltinTy` —
// missing a choice case entirely. So a `Wrap?` field never took
// lowerTypeRef's nullable-value-type branch; it fell through to the LATER
// Named/Choice branch, which returns the BARE (non-nullable) choiceStruct
// regardless of `rt->nullable` — directly contradicting that branch's own
// (long-standing, never-actually-true) comment, "the nullable `C?` wraps
// this struct in nullableValueStructTy's `{ i8 hasValue, <choice
// struct> }`". The bare choice struct's field 0 is an `i32` tag
// (choiceStruct's discriminator) rather than the `i8 hasValue` flag every
// nullable-consuming site assumes, so the first site that tested the
// (non-existent) hasValue tag hit an LLVM ICmp type-mismatch assertion
// (comparing the `i32` tag against an `i8` constant) and aborted the
// compiler.
//
// VERIFY-FIRST: `record Root(wrap: Wrap?): ISerializable, IDeserializable`
// with `p.Serialize(s);` — confirmed via `bezelc -c` on unmodified main:
// `Assertion 'getOperand(0)->getType() == getOperand(1)->getType() ...'
// failed` (SIGABRT, core dump). Confirmed pre-existing (independent of
// #2425/#2428/#2448) via patch isolation.
//
// Fix: `isNullableChoiceTy` (mirroring `isNullableRecordTy` exactly) and a
// matching branch in `nullableValueStructTy` (mirroring the record branch,
// reusing `choiceStruct` + `RecordSubstGuard` for the generic case) close
// the gap — `isNullableValueTy` now includes choices, so `lowerTypeRef`
// takes the correct tagged-struct branch for every `C?` before ever
// reaching the (now genuinely non-nullable-only) later Named/Choice
// branch. This is a general layout fix, not serialization-specific — it
// affects every consumer of `isNullableValueTy`/`nullableValueStructTy`
// (locals, params, fields, returns, equality, `?.`/`??`, boxing), so this
// file also pins a NON-serialization nullable-choice local (equality
// against `null`, unwrap-and-match) to demonstrate the fix's actual
// scope, not just the one call site (Serialize) that happened to crash
// first.
//
// LEAK NOTE (NOT part of this fix's scope, pre-existing and unrelated):
// a nullable choice value's payload string is not covered by ANY of the
// existing scope-end release optimizations (#2115's direct-construction
// release, #2405/#2425's deserialize disposal-scope recursion — both
// explicitly exclude a nullable member/local, matching the SAME exclusion
// nullable records already have) — so a present (non-null) nullable
// choice still LEAKS its payload string, both as a record component and
// as a plain local. This is the same "safe leak, not a crash" trade-off
// the whole codebase already accepts for nullable value types generally;
// #2447 is specifically about the CRASH, and closing the leak is a
// separate, pre-existing, un-worsened gap — pinned below via
// runAotCheckedLenient (no crash), not claimed leak-free.

#include "support/aot_support.hpp"   // runAotChecked / runAotCheckedLenient
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {
const char* kImport = "import System.Serialization; ";
}  // namespace

// ===========================================================================
// The issue's own minimal repro: a record component of NULLABLE
// value-like-choice type, Serialize only (no Deserialize needed to trigger
// the crash). Pre-fix: bezelc aborts (LLVM ICmp assertion). Post-fix:
// compiles and runs cleanly (nothing is read, so nothing can leak either —
// asserted via the full LSan harness).
// ===========================================================================
BEZEL_TEST(nullable_choice_layout_2447,
           serialize_nullable_choice_record_component_no_crash_spec_35_9) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "choice Wrap: ISerializable, IDeserializable "
        "  Holder(tag: string), Empty; end Wrap; "
        "record Root(wrap: Wrap?): ISerializable, IDeserializable end Root; "
        "class C public static method Main(): int "
        "  var p: Root := new Root(Wrap.Holder(\"hi\")); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  return 42; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // false pre-fix: SIGABRT (LLVM assertion)
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// Full round trip with a PRESENT (non-null) nullable choice component:
// value round-trips correctly (Holder's payload string matches). LSan is
// NOT asserted clean here — the deserialized payload leaks (the pre-
// existing, documented nullable-exclusion gap noted above) — the pin is
// specifically "no crash", via runAotCheckedLenient.
// ===========================================================================
BEZEL_TEST(nullable_choice_layout_2447,
           present_nullable_choice_roundtrip_value_correct_spec_35_9) {
    auto r = runAotCheckedLenient(
        std::string("module M; ") + kImport +
        "choice Wrap: ISerializable, IDeserializable "
        "  Holder(tag: string), Empty; end Wrap; "
        "record Root(wrap: Wrap?): ISerializable, IDeserializable end Root; "
        "class C public static method Main(): int "
        "  var p: Root := new Root(Wrap.Holder(\"present-nullable-choice-value\")); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var back: Root := Root.Deserialize(d); "
        "  if back.wrap = null then return 1; end; "
        "  var tag: string := (case back.wrap! of Holder(var t) => t, Empty => \"\"); "
        "  if tag <> \"present-nullable-choice-value\" then return 2; end; "
        "  return 42; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // false pre-fix: SIGABRT (LLVM assertion)
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// Full round trip with a NULL nullable choice component: the null state
// itself round-trips (WriteNull / TryReadNull), and — since a null carries
// no payload to leak — this program IS fully LSan-clean, asserted via the
// full harness.
// ===========================================================================
BEZEL_TEST(nullable_choice_layout_2447,
           null_nullable_choice_roundtrip_clean_spec_35_9) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "choice Wrap: ISerializable, IDeserializable "
        "  Holder(tag: string), Empty; end Wrap; "
        "record Root(wrap: Wrap?): ISerializable, IDeserializable end Root; "
        "class C public static method Main(): int "
        "  var p: Root := new Root(null); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var back: Root := Root.Deserialize(d); "
        "  if back.wrap <> null then return 1; end; "
        "  return 42; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// The fix is general (isNullableValueTy/nullableValueStructTy), not
// serialization-specific: a plain NULLABLE CHOICE LOCAL (no
// ISerializable/IDeserializable involved at all) exercises the same
// tagged-struct layout via equality-against-null and the null-forgiving
// unwrap (`!`, §17.8) + case match. Pinned "no crash" (the payload-string
// leak on the present local is the SAME pre-existing, unrelated
// local-release scope gap noted above — not asserted leak-free).
// ===========================================================================
BEZEL_TEST(nullable_choice_layout_2447,
           general_nullable_choice_local_equality_and_unwrap_no_crash_spec_35_9) {
    auto r = runAotCheckedLenient(
        "module M; choice Wrap Holder(tag: string), Empty; end Wrap; "
        "class C public static method Main(): int "
        "  var w1: Wrap? := null; "
        "  var w2: Wrap? := Wrap.Holder(\"hello\" + \"-general-nullable-choice\"); "
        "  if w1 <> null then return 1; end; "
        "  if w2 = null then return 2; end; "
        "  var tag: string := (case w2! of Holder(var t) => t, Empty => \"\"); "
        "  if tag <> \"hello-general-nullable-choice\" then return 3; end; "
        "  return 42; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // false pre-fix: SIGABRT (LLVM assertion)
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
