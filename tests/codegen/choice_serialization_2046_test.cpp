// Codegen tests — §9.8 choice serialization synthesis, the `$case`-discriminated
// wire form (issue #2046, spec §9.8 "Choice serialization (v0.200)", §35.4).
//
// A value-like choice that declares `: ISerializable`/`: IDeserializable` has its
// Serialize/Deserialize synthesized.  The wire form is an object whose leading
// field is the case discriminator `"$case"` (the case's declared name, or its
// `serialized "<name>"` rename), followed by the active case's payload fields by
// name in declaration order.  On read the discriminator is consumed by case
// dispatch FIRST, before any payload binding.
//
// These are round-trip + fault behavior tests: each Main returns a small int the
// AOT harness reads as the process exit code (values kept < 256).  Round-trips
// that allocate (string payloads, nested choices) go through runAotChecked-style
// exit-code assertions via runAot; the disposal of the value-like choice value is
// trivial (no owned payloads), so a plain runAot exit-code check suffices.
//
// APIs verified against Bezel_Standard_Library.md: ISerializer/IDeserializer
// (§2.14), BinarySerializer/BinaryDeserializer/JsonSerializer/JsonDeserializer
// (§2.15), SerializationException (§2.15).  BinaryDeserializer is fed
// `bytes.AsReadOnlySpan()` (§1358).
//
// Spec refs: §9.8 (choice subsection, v0.200), §35.4, stdlib §2.14/§2.15.
// Issue #2046.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

const char* kImport = "import System.Serialization; ";

} // namespace

// ===========================================================================
// §9.8: round-trip per payload category.
// ===========================================================================

// §9.8: a value payload (int) round-trips through the binary format.  The
// canonical value-like choice: Opt.Some(int) / Opt.None.
BEZEL_TEST(codegen_choice_ser_2046, int_payload_roundtrip_binary_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "choice Opt: ISerializable, IDeserializable Some(value: int), None; end Opt; "
        "class C public static method Main(): int "
        "  var o := Opt.Some(42); "
        "  using var s := new BinarySerializer(); "
        "  o.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var v := Opt.Deserialize(d); "
        "  return (case v of Some(var x) => x, None => 0); "
        "end Main; end C;"), 42);
}

// §9.8: a string payload round-trips through the binary format.
BEZEL_TEST(codegen_choice_ser_2046, string_payload_roundtrip_binary_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "choice Msg: ISerializable, IDeserializable Text(s: string), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var m := Msg.Text(\"hello\"); "
        "  using var ser := new BinarySerializer(); "
        "  m.Serialize(ser); "
        "  using var bytes := ser.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var v := Msg.Deserialize(d); "
        "  return (case v of Text(var t) when t = \"hello\" => 1, _ => 0); "
        "end Main; end C;"), 1);
}

// §9.8: a payload-free case round-trips (writes the object with only the
// discriminator; "A payload-free case writes the object with only the
// discriminator").
BEZEL_TEST(codegen_choice_ser_2046, payload_free_case_roundtrip_binary_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "choice Opt: ISerializable, IDeserializable Some(value: int), None; end Opt; "
        "class C public static method Main(): int "
        "  var o := Opt.None; "
        "  using var s := new BinarySerializer(); "
        "  o.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var v := Opt.Deserialize(d); "
        "  return (case v of None => 7, Some(_) => 0); "
        "end Main; end C;"), 7);
}

// §9.8 EXACT WIRE, multi-field: discriminator FIRST, then payload fields by
// name in DECLARATION order — the round-trip tests above cannot see ordering;
// this pin can (batch-B review rider). v0.200.
BEZEL_TEST(codegen_choice_ser_2046, multi_field_exact_wire_order_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "choice FetchOutcome: ISerializable, IDeserializable "
        "  NotModified, Failed(status: int, message: string); end FetchOutcome; "
        "class C public static method Main(): int "
        "  var f := FetchOutcome.Failed(504, \"up\"); "
        "  using var s := new JsonSerializer(); "
        "  f.Serialize(s); "
        "  return (if s.ToString() = "
        "    \"{\\\"$case\\\":\\\"Failed\\\",\\\"status\\\":504,\\\"message\\\":\\\"up\\\"}\" "
        "    then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.8: a multi-field value payload (int + string) round-trips through JSON.
BEZEL_TEST(codegen_choice_ser_2046, multi_field_payload_roundtrip_json_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "choice FetchOutcome: ISerializable, IDeserializable "
        "  NotModified, Failed(status: int, message: string); end FetchOutcome; "
        "class C public static method Main(): int "
        "  var f := FetchOutcome.Failed(504, \"upstream timeout\"); "
        "  using var s := new JsonSerializer(); "
        "  f.Serialize(s); "
        "  using var d := new JsonDeserializer(s.ToString()); "
        "  var v := FetchOutcome.Deserialize(d); "
        "  return (case v of "
        "    Failed(var st, var m) when st = 504 and m = \"upstream timeout\" => 1, "
        "    _ => 0); "
        "end Main; end C;"), 1);
}

// §9.8: a record payload round-trips (a serialization-eligible record is a
// record-admissible payload).
BEZEL_TEST(codegen_choice_ser_2046, record_payload_roundtrip_binary_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Pt(X: int, Y: int): ISerializable, IDeserializable; "
        "choice Shape: ISerializable, IDeserializable Dot(at: Pt), Origin; end Shape; "
        "class C public static method Main(): int "
        "  var sh := Shape.Dot(new Pt(3, 4)); "
        "  using var s := new BinarySerializer(); "
        "  sh.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var v := Shape.Deserialize(d); "
        "  return (case v of Dot(var p) when p.X = 3 and p.Y = 4 => 1, _ => 0); "
        "end Main; end C;"), 1);
}

// §9.8: a nested value-like choice payload round-trips (recursively value-shaped
// storage; the nested choice reconstructs via its own `$case` object form).
BEZEL_TEST(codegen_choice_ser_2046, nested_choice_payload_roundtrip_binary_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "choice Inner: ISerializable, IDeserializable Lo(v: int), Hi(v: int); end Inner; "
        "choice Outer: ISerializable, IDeserializable Wrap(inner: Inner), Bare; end Outer; "
        "class C public static method Main(): int "
        "  var o := Outer.Wrap(Inner.Hi(7)); "
        "  using var s := new BinarySerializer(); "
        "  o.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var v := Outer.Deserialize(d); "
        "  return (case v of "
        "    Wrap(var i) => (case i of Lo(var x) => x, Hi(var x) => 100 + x), "
        "    Bare => 0); "
        "end Main; end C;"), 107);
}

// ===========================================================================
// §9.8: the serialized clause — case rename and payload rename.
// ===========================================================================

// §9.8: the `serialized "<name>"` clause on a CASE renames the `$case`
// discriminator value.  The exact wire form is asserted so the discriminator
// placement and rename are both pinned.
BEZEL_TEST(codegen_choice_ser_2046, case_rename_wire_form_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "choice Shape: ISerializable, IDeserializable "
        "  Named(label: string) serialized \"named_case\", Origin; end Shape; "
        "class C public static method Main(): int "
        "  var b := Shape.Named(\"hello\"); "
        "  using var s := new JsonSerializer(); "
        "  b.Serialize(s); "
        "  return (if s.ToString() = "
        "    \"{\\\"$case\\\":\\\"named_case\\\",\\\"label\\\":\\\"hello\\\"}\" "
        "    then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.8: a case rename round-trips (deserialize accepts the renamed discriminator).
BEZEL_TEST(codegen_choice_ser_2046, case_rename_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "choice Shape: ISerializable, IDeserializable "
        "  Named(label: string) serialized \"named_case\", Origin; end Shape; "
        "class C public static method Main(): int "
        "  var b := Shape.Named(\"hi\"); "
        "  using var s := new JsonSerializer(); "
        "  b.Serialize(s); "
        "  using var d := new JsonDeserializer(s.ToString()); "
        "  var v := Shape.Deserialize(d); "
        "  return (case v of Named(var l) when l = \"hi\" => 1, _ => 0); "
        "end Main; end C;"), 1);
}

// §9.8: a payload-field rename ("body") + declaration-order field layout.  The
// exact wire form pins deterministic field order (discriminator first, then the
// payload fields in declaration order).
BEZEL_TEST(codegen_choice_ser_2046, payload_rename_wire_form_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "choice Msg: ISerializable, IDeserializable "
        "  Note(text: string serialized \"body\"), Ping; end Msg; "
        "class C public static method Main(): int "
        "  var a := Msg.Note(\"hello\"); "
        "  using var s := new JsonSerializer(); "
        "  a.Serialize(s); "
        "  return (if s.ToString() = "
        "    \"{\\\"$case\\\":\\\"Note\\\",\\\"body\\\":\\\"hello\\\"}\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.8: `serialized optional` on a payload field — a missing field on read yields
// default(T) instead of raising (orthogonal to the discriminator).
BEZEL_TEST(codegen_choice_ser_2046, payload_optional_missing_reads_default_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "choice Msg: ISerializable, IDeserializable "
        "  Note(text: string, author: string serialized optional), Ping; end Msg; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"$case\\\":\\\"Note\\\",\\\"text\\\":\\\"hi\\\"}\"); "
        "  var v := Msg.Deserialize(d); "
        "  return (case v of Note(var t, var au) when t = \"hi\" and au = null => 1, _ => 0); "
        "end Main; end C;"), 1);
}

// ===========================================================================
// §9.8: the fault cases the spec enumerates — each raises SerializationException.
// ===========================================================================

// §9.8: "an undeclared discriminator string raises SerializationException".
BEZEL_TEST(codegen_choice_ser_2046, undeclared_discriminator_faults_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "choice Shape: ISerializable, IDeserializable Origin, Dot(x: int); end Shape; "
        "class C public static method Main(): int "
        "  var caught := 0; "
        "  try "
        "    using var d := new JsonDeserializer(\"{\\\"$case\\\":\\\"Nonexistent\\\"}\"); "
        "    var v := Shape.Deserialize(d); "
        "    caught := (case v of _ => 0); "
        "  catch SerializationException => caught := 1; "
        "  end; "
        "  return caught; "
        "end Main; end C;"), 1);
}

// §9.8: "an object that does not lead with the discriminator [raises
// SerializationException] (dispatch must select the case before component
// binding can begin)".
BEZEL_TEST(codegen_choice_ser_2046, missing_leading_discriminator_faults_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "choice Shape: ISerializable, IDeserializable Origin, Dot(x: int); end Shape; "
        "class C public static method Main(): int "
        "  var caught := 0; "
        "  try "
        "    using var d := new JsonDeserializer(\"{\\\"x\\\":1}\"); "
        "    var v := Shape.Deserialize(d); "
        "    caught := (case v of _ => 0); "
        "  catch SerializationException => caught := 1; "
        "  end; "
        "  return caught; "
        "end Main; end C;"), 1);
}

// §9.8: a missing REQUIRED payload field (present neither in the input nor as
// `serialized optional`) raises SerializationException (the standard
// missing-field rule applied to the selected case's payloads).
BEZEL_TEST(codegen_choice_ser_2046, missing_required_payload_faults_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "choice Msg: ISerializable, IDeserializable "
        "  Note(text: string, count: int), Ping; end Msg; "
        "class C public static method Main(): int "
        "  var caught := 0; "
        "  try "
        "    using var d := new JsonDeserializer(\"{\\\"$case\\\":\\\"Note\\\",\\\"text\\\":\\\"x\\\"}\"); "
        "    var v := Msg.Deserialize(d); "
        "    caught := (case v of _ => 0); "
        "  catch SerializationException => caught := 1; "
        "  end; "
        "  return caught; "
        "end Main; end C;"), 1);
}

// §9.8: a payload type mismatch (a string where an int is declared) raises
// SerializationException (a structural fault regardless of policy).
BEZEL_TEST(codegen_choice_ser_2046, payload_type_mismatch_faults_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "choice Shape: ISerializable, IDeserializable Origin, Dot(x: int); end Shape; "
        "class C public static method Main(): int "
        "  var caught := 0; "
        "  try "
        "    using var d := new JsonDeserializer(\"{\\\"$case\\\":\\\"Dot\\\",\\\"x\\\":\\\"nope\\\"}\"); "
        "    var v := Shape.Deserialize(d); "
        "    caught := (case v of _ => 0); "
        "  catch SerializationException => caught := 1; "
        "  end; "
        "  return caught; "
        "end Main; end C;"), 1);
}

// §9.8: a choice all of whose cases are payload-free still writes the object form
// (uniformity).  The exact wire form pins the discriminator-only object.
BEZEL_TEST(codegen_choice_ser_2046, all_payload_free_writes_object_form_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "choice Flag: ISerializable, IDeserializable On, Off; end Flag; "
        "class C public static method Main(): int "
        "  var f := Flag.On; "
        "  using var s := new JsonSerializer(); "
        "  f.Serialize(s); "
        "  return (if s.ToString() = \"{\\\"$case\\\":\\\"On\\\"}\" then 1 else 0); "
        "end Main; end C;"), 1);
}
