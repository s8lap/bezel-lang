// Codegen — issue #2405 / spec §9.8 ("Serialization synthesis") + §9.7
// ("Records and disposal") + §28.3/§28.9 (exactly-once reclamation), docs
// revision v0.235.
//
// #2398 fixed `emitRecordDeserialize` / `emitChoiceDeserialize` so an
// already-read component/payload slot — a `string`, or a NESTED RECORD
// component that itself owns a `string` — is disposed when a LATER
// sibling's read throws. The nested-record half of that fix reused the
// existing #1747 `emitRecordStringFieldReleases` helper, which released
// only a record's OWN DIRECT `string` storage members: ONE level. #2405 is
// that one-level scope showing through at 2+ levels of nesting: a
// component whose type is `Mid` (a record), where `Mid` itself has a
// component of type `Leaf` (another record) owning a `string` —
// `Root(mid: Mid, second: int)`, `Mid(leaf: Leaf)`, `Leaf(tag: string)` —
// still leaked `Leaf.tag` when `Root.second`'s read threw, because
// `emitRecordStringFieldReleases(rootMidSlot, MidTypeSymbol)` walked
// `Mid`'s OWN storage (no direct string field — `leaf` is record-typed)
// and stopped there.
//
// VERIFY-FIRST: reproduced exactly as filed against this lane's rebase
// (main @ 647fc6b, i.e. AFTER #2399 landed) — the issue's own repro leaks
// "60 byte(s) leaked in 2 allocation(s)" under LSan (32-byte bz_string
// header + 28-byte UTF-8 data for "deep-inner-tag-value-string", matching
// exactly).
//
// Fix: `recordHasStringFields` and `emitRecordStringFieldReleases_impl`
// (src/codegen/LlvmBackend_impl.hpp) now RECURSE into a non-nullable
// nested-record storage member (a nested record is embedded INLINE — a
// GEP off the same outer struct address reaches it, no load/indirection),
// to an ARBITRARY depth, not just one level. This is issue #2405's own
// "Option 1" (make the shared helper genuinely recursive), chosen over
// "Option 2" (a narrower depth-N walk scoped just to the #2398
// registration site) because the recursion terminates provably: sema
// rejects a record that transitively contains itself by value at the
// DECLARATION site (`TypeResolution_Checks.cpp`'s
// `recordValueContainsSelf`, "cannot recursively contain itself by value",
// §9), so the nested-record type graph reachable from any record type is
// a finite DAG — no cycle guard needed, exactly mirroring
// `emitStructEquality`'s existing unconditional recursion into nested
// records for `=`/`<>` (`LlvmBackend_Layout.cpp`).
//
// IMPORTANT — a shared-helper trap this fix caught in its own first draft:
// `recordHasStringFields` / `isRecordWithStringFieldsTypeRef` are NOT
// private to the #2398/#2405 deserialize disposal-scope registration —
// they are ALSO the gate `LlvmBackend_Setup.cpp` uses for the SEPARATE
// #1747 mechanism (`var p := Mk();` releasing a non-escaping record-call-
// result local's string fields at scope end). Recursing here therefore
// ALSO extends #1747's own release to 2+ levels of nesting — verified
// SAFE (not a double-free/UAF) because #1747's own landed commit message
// establishes that no separate per-level retain step exists to fall out
// of sync with ("no extra +1 mint is needed ... the record VALUE carries
// its fields' +1 references to the caller") — a fact that holds
// compositionally at any depth, since a by-value struct copy/embed never
// touches a refcount. Confirmed empirically (see the lane report): a
// `var p := Mk();` binding a 2-level-nested-string record, PRE-fix, leaked
// 62 bytes (an interpolated/computed string, not a literal — a literal
// field is a compile-time sentinel that never allocates regardless); with
// this fix, it is LSan-clean; the disqualified (escaping,
// `Use(Mk())`-shaped) control still shows the pre-existing SAFE
// leak-not-crash behaviour (no UAF), unchanged by this fix.
//
// A value-like CHOICE component/payload (nested at any depth, in a record
// or directly) has no equivalent release walker at all yet — a separate,
// documented residual gap, #2425 filed.
//
// Adversarial grid covered (full detail in the lane report):
//   * record component nesting depth 1 (pre-existing, must stay fixed),
//     2 (the issue's own repro), and 3 (this file, proving arbitrary
//     depth, not merely "one more level").
//   * the SAME 2-level nesting reached via a choice PAYLOAD field (the
//     issue's title names both record and choice).
//   * clean (non-truncated) round-trips at depth 3, for both the throw
//     path (disposal) and the ordinary success path (construction/
//     `with`-style consumption of the recursively-registered slots must
//     not itself leak or double-free).
//   * the #1747 `var p := Mk();` bonus fix above, plus its escape-safety
//     control (no UAF regression) — verified via probe, not re-pinned as
//     a gtest here (out of THIS issue's literal scope; #1747's own test
//     file already covers the flat-record shape this fix does not change).

#include "support/aot_support.hpp"   // runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
const char* kImport = "import System.Serialization; ";
}  // namespace

// ===========================================================================
// The issue's own exact repro: Root(mid: Mid, second: int), Mid(leaf: Leaf),
// Leaf(tag: string) — Root.second's truncated read throws; Leaf.tag (2
// levels deep, successfully read before the throw) must be disposed, not
// orphaned.
// ===========================================================================
BEZEL_TEST(serialization_nested_record_disposal_2405,
           record_two_levels_deep_string_disposed_on_sibling_throw_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record Leaf(tag: string): ISerializable, IDeserializable end Leaf; "
        "record Mid(leaf: Leaf): ISerializable, IDeserializable end Mid; "
        "record Root(mid: Mid, second: int): ISerializable, IDeserializable "
        "end Root; "
        "class C public static method Main(): int "
        "  var p: Root := new Root(new Mid(new Leaf(\"deep-inner-tag-value-string\")), 12345); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 3; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var back: Root := Root.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // false pre-fix: 60-byte / 2-alloc leak
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// Three levels deep: Root(outer: Outer, second: int), Outer(mid: Mid),
// Mid(leaf: Leaf), Leaf(tag: string) — proves the fix is genuinely
// recursive (arbitrary depth), not merely "one level deeper" than #2398's
// original scope.
// ===========================================================================
BEZEL_TEST(serialization_nested_record_disposal_2405,
           record_three_levels_deep_string_disposed_on_sibling_throw_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record Leaf(tag: string): ISerializable, IDeserializable end Leaf; "
        "record Mid(leaf: Leaf): ISerializable, IDeserializable end Mid; "
        "record Outer(mid: Mid): ISerializable, IDeserializable end Outer; "
        "record Root(outer: Outer, second: int): ISerializable, IDeserializable "
        "end Root; "
        "class C public static method Main(): int "
        "  var p: Root := new Root(new Outer(new Mid(new Leaf(\"deep-inner-tag-value-string\"))), 12345); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 3; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var back: Root := Root.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// The same two-levels-deep shape reached via a CHOICE payload field
// (Msg.Text(mid: Mid, second: int)) rather than a record component — the
// issue's title names both record AND choice.
// ===========================================================================
BEZEL_TEST(serialization_nested_record_disposal_2405,
           choice_payload_two_levels_deep_string_disposed_on_sibling_throw_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record Leaf(tag: string): ISerializable, IDeserializable end Leaf; "
        "record Mid(leaf: Leaf): ISerializable, IDeserializable end Mid; "
        "choice Msg: ISerializable, IDeserializable "
        "  Text(mid: Mid, second: int), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var m := Msg.Text(new Mid(new Leaf(\"deep-inner-tag-value-string\")), 12345); "
        "  using var s := new BinarySerializer(); "
        "  m.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 3; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var back := Msg.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // false pre-fix: same 60-byte / 2-alloc leak
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// Clean, full-read control at three levels deep (no truncation): round-
// trips correctly (value equality on the deepest string AND the sibling
// int) and the fix's now-recursive per-slot disposal registration — popped
// WITHOUT running its disposals once construction begins — does not itself
// leak or double-free on the ordinary success path.
// ===========================================================================
BEZEL_TEST(serialization_nested_record_disposal_2405,
           clean_three_levels_deep_roundtrip_no_leak_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record Leaf(tag: string): ISerializable, IDeserializable end Leaf; "
        "record Mid(leaf: Leaf): ISerializable, IDeserializable end Mid; "
        "record Outer(mid: Mid): ISerializable, IDeserializable end Outer; "
        "record Root(outer: Outer, second: int): ISerializable, IDeserializable "
        "end Root; "
        "class C public static method Main(): int "
        "  var p: Root := new Root(new Outer(new Mid(new Leaf(\"deep-inner-tag-value-string\"))), 12345); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var back: Root := Root.Deserialize(d); "
        "  if back.outer.mid.leaf.tag <> \"deep-inner-tag-value-string\" then return 1; end; "
        "  if back.second <> 12345 then return 2; end; "
        "  return 42; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
