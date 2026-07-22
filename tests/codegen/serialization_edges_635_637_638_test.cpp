// Codegen tests — §9.8 serialization edge cases: issues #635, #637, #638.
//
// #638: nullable primitive T? fields (int?, bool?) must branch on the null
//       marker rather than attempting to sign-extend the whole nullable struct.
//       Root cause: emitSerializeField/emitDeserializeField checked K::Primitive
//       before checking fieldTy->nullable, so int? hit the primitive branch with
//       a {i8,i32} struct value → LLVM verifier: "SExt only operates on integer".
//
// #637: nullable string? must write a null marker (not a zero-length string) on
//       the null path, and must read the null marker back as null (not "").
//       Root cause: same ordering bug — string? is K::Primitive, so the nullable
//       branch was never reached in either Serialize or Deserialize.
//
// #635: multi-byte UTF-8 strings (CJK, emoji) corrupt through binary round-trip.
//       Root cause: bz_binder_read_bz_string created strings with width=1 and
//       len = byte_count, but the rest of the runtime treats len as code-point
//       count; emitStringEq's length comparison then compared code-point count
//       against byte count → mismatch even for perfectly-encoded strings.
//
// Spec refs: §9.8, stdlib §2.14, §2.15.
// Wire-format choice (§2.15 unspecified for nullable): null marker = single 0x00
// byte (bz_binser_write_null / bz_binder_try_read_null), consistent across all
// nullable types (reference and value). Non-null value follows immediately after.
// This matches the pre-existing string? null marker path in the reference
// (nullable) branch for pointer types.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

const char* kImp = "import System.Serialization; ";

} // namespace

// ===========================================================================
// #638: nullable primitive (int?) round-trips — null and non-null
// ===========================================================================

// §9.8 #638: int? field set to null serializes as null marker; deserializes back
// to null (not 0 or any garbage). Compile used to crash with LLVM IR-verify error.
BEZEL_TEST(codegen_serialization_edges_638, nullable_int_null_roundtrip_spec_9_8_issue_638) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImp +
        "record R(Y: int?): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var r := new R(null); "
        "  using var s := new BinarySerializer(); "
        "  r.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var r2 := R.Deserialize(d); "
        "  return (if r2.Y = null then 42 else 91); "
        "end Main; end C;"), 42);
}

// §9.8 #638: int? field set to a non-null value round-trips correctly.
BEZEL_TEST(codegen_serialization_edges_638, nullable_int_nonnull_roundtrip_spec_9_8_issue_638) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImp +
        "record R(Y: int?): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var r := new R(7); "
        "  using var s := new BinarySerializer(); "
        "  r.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var r2 := R.Deserialize(d); "
        "  return (if r2.Y = 7 then 42 else 91); "
        "end Main; end C;"), 42);
}

// §9.8 #638: bool? field round-trips for both null and non-null.
BEZEL_TEST(codegen_serialization_edges_638, nullable_bool_null_roundtrip_spec_9_8_issue_638) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImp +
        "record R(F: bool?): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var r := new R(null); "
        "  using var s := new BinarySerializer(); "
        "  r.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var r2 := R.Deserialize(d); "
        "  return (if r2.F = null then 42 else 91); "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_serialization_edges_638, nullable_bool_true_roundtrip_spec_9_8_issue_638) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImp +
        "record R(F: bool?): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var r := new R(true); "
        "  using var s := new BinarySerializer(); "
        "  r.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var r2 := R.Deserialize(d); "
        "  return (if r2.F = true then 42 else 91); "
        "end Main; end C;"), 42);
}

// §9.8 #638: combined record with nullable int, bool, and a normal int — all round-trip.
BEZEL_TEST(codegen_serialization_edges_638, combined_nullable_and_plain_roundtrip_spec_9_8_issue_638) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImp +
        "record P(X: int, Y: int?, Z: bool?): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var p := new P(5, null, true); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var p2 := P.Deserialize(d); "
        "  if p2.X <> 5 then return 91; end; "
        "  if p2.Y <> null then return 92; end; "
        "  if p2.Z <> true then return 93; end; "
        "  return 42; "
        "end Main; end C;"), 42);
}

// ===========================================================================
// #637: nullable string? — null path must produce null, not ""
// ===========================================================================

// §9.8 #637: string? null round-trip — Serialize writes null marker, Deserialize
// reads null marker back as null (not "" which was the pre-fix behaviour).
BEZEL_TEST(codegen_serialization_edges_637, nullable_string_null_roundtrip_spec_9_8_issue_637) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImp +
        "record R(Y: string?): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var r := new R(null); "
        "  using var s := new BinarySerializer(); "
        "  r.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var r2 := R.Deserialize(d); "
        "  return (if r2.Y = null then 42 else 91); "
        "end Main; end C;"), 42);
}

// §9.8 #637: string? non-null round-trips correctly.
BEZEL_TEST(codegen_serialization_edges_637, nullable_string_nonnull_roundtrip_spec_9_8_issue_637) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImp +
        "record R(Y: string?): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var r := new R(\"hello\"); "
        "  using var s := new BinarySerializer(); "
        "  r.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var r2 := R.Deserialize(d); "
        "  return (if r2.Y = \"hello\" then 42 else 91); "
        "end Main; end C;"), 42);
}

// §9.8 #637: serialized optional missing field → default(string?) = null, not "".
// Per §9.8 / §2.15: a `serialized optional` field absent from the wire stream
// deserializes as default(T?) = null.
BEZEL_TEST(codegen_serialization_edges_637, serialized_optional_missing_field_is_null_spec_9_8_issue_637) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImp +
        "record Item(Name: string, Tag: string? serialized optional): "
        "  ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var m := new Item(\"thing\", null); "
        "  using var s := new BinarySerializer(); "
        "  m.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var back := Item.Deserialize(d); "
        "  if back.Name <> \"thing\" then return 91; end; "
        "  return (if back.Tag = null then 42 else 92); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// #635: multi-byte UTF-8 round-trip — CJK, emoji, mixed
// ===========================================================================

// §9.8 #635: CJK string round-trip. Pre-fix: emitStringEq compared code-point
// count (6) against byte count (18 for Japanese UTF-8) → always false → rc=91.
BEZEL_TEST(codegen_serialization_edges_635, cjk_string_roundtrip_spec_9_8_issue_635) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImp +
        "record Msg(Text: string): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var m := new Msg(\"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88\"); " // 日本語テスト
        "  using var s := new BinarySerializer(); "
        "  m.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var back := Msg.Deserialize(d); "
        "  return (if back.Text = \"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88\" then 42 else 91); " // 日本語テスト
        "end Main; end C;"), 42);
}

// §9.8 #635: ASCII string still round-trips correctly after the fix (regression guard).
BEZEL_TEST(codegen_serialization_edges_635, ascii_string_roundtrip_still_correct_spec_9_8_issue_635) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImp +
        "record Msg(Text: string): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var m := new Msg(\"hello world\"); "
        "  using var s := new BinarySerializer(); "
        "  m.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var back := Msg.Deserialize(d); "
        "  return (if back.Text = \"hello world\" then 42 else 91); "
        "end Main; end C;"), 42);
}

// §9.8 #635: mixed ASCII + multi-byte UTF-8 round-trip.
BEZEL_TEST(codegen_serialization_edges_635, mixed_ascii_multibyte_roundtrip_spec_9_8_issue_635) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImp +
        "record R(S: string): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        // "a日b" — ASCII + CJK + ASCII
        "  var r := new R(\"a\xE6\x97\xA5" "b\"); "
        "  using var s := new BinarySerializer(); "
        "  r.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var r2 := R.Deserialize(d); "
        "  return (if r2.S = \"a\xE6\x97\xA5" "b\" then 42 else 91); "
        "end Main; end C;"), 42);
}

// ===========================================================================
// Combined record: all edge cases together
// ===========================================================================

// §9.8 #635/#637/#638: combined record with nullable int, nullable string,
// and a multi-byte UTF-8 string field — all round-trip correctly.
BEZEL_TEST(codegen_serialization_edges_combined, all_three_fixes_combined_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImp +
        "record All("
        "  Tag: int?, "
        "  Label: string?, "
        "  Name: string): "
        "  ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var a := new All(null, null, \"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\"); " // 日本語
        "  using var s := new BinarySerializer(); "
        "  a.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var a2 := All.Deserialize(d); "
        "  if a2.Tag <> null then return 91; end; "
        "  if a2.Label <> null then return 92; end; "
        "  return (if a2.Name = \"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\" then 42 else 93); " // 日本語
        "end Main; end C;"), 42);
}
